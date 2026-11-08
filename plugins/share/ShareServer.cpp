#include "ShareServer.h"

#include "ShareStore.h"
#include "ShareWeb.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>

namespace share {

namespace {

/// 请求行 + 头部的字节上限。正常浏览器一个请求头不到 2 KB，
/// 64 KB 已经是病态值；不设上限就会被"发一堆头不结束"的连接骗走内存。
constexpr int kMaxHeaderBytes = 64 * 1024;

/// JSON 请求体的上限（文本发送走这条）。共享文本本身限制 16 KB，
/// 给 512 KB 是留给 JSON 转义与未来加字段的余量。
constexpr qint64 kMaxJsonBodyBytes = 512 * 1024;

/// 下载时 socket 写缓冲的高水位：积压超过它就暂停读文件，等 bytesWritten 再续。
/// 不设这个的话，一个 2 GB 文件的下载会把整个文件读进 socket 缓冲区（内存直接爆）。
constexpr qint64 kSocketHighWater = 512 * 1024;

/// 每次从磁盘读的块大小。64 KB 是吞吐与调度粒度的折中。
constexpr qint64 kDownloadChunk = 64 * 1024;

///
/// 一条 TCP 连接 = 一个 HTTP 会话。
///
/// 状态机只有三段：读头 → 读体（可选）→ 路由。keep-alive 下这三段会被反复走，
/// 所以所有状态都在 resetRequest() 里统一清干净，别在分支里零散复位。
///
/// 刻意不带 Q_OBJECT：这个类不需要自定义信号，所有连接都用 lambda / 成员函数指针，
/// 省掉一份 moc 产物，也避免"内部类带 Q_OBJECT 忘了 include .moc"这类坑。
///
class HttpConnection : public QObject
{
public:
    explicit HttpConnection(QTcpSocket *socket, QObject *parent)
        : QObject(parent)
        , m_socket(socket)
    {
        m_socket->setParent(this);
        connect(m_socket, &QTcpSocket::readyRead, this, [this] { onReadyRead(); });
        connect(m_socket, &QTcpSocket::bytesWritten, this, [this](qint64) { onBytesWritten(); });
        connect(m_socket, &QTcpSocket::disconnected, this, &QObject::deleteLater);
    }

    ~HttpConnection() override
    {
        // 传到一半就断开：让 QSaveFile 丢掉临时文件，别在共享目录里留半截垃圾。
        if (m_upload)
            m_upload->cancelWriting();
    }

private:
    // ---------------------------------------------------------------- 主循环

    void onReadyRead()
    {
        const qint64 available = m_socket->bytesAvailable();
        if (available <= 0)
            return;
        // 单次最多读 256 KB：上传 2 GB 文件时若 readAll()，整个 body 会一次性进内存。
        m_inBuf += m_socket->read(qMin<qint64>(available, 256 * 1024));
        processBuffer();
    }

    /// 从缓冲区里尽可能多地推进请求。返回时要么数据不够、要么连接已关闭。
    void processBuffer()
    {
        while (!m_closing) {
            if (!m_headerDone) {
                if (!parseHead())
                    return;
                if (m_closing)
                    return;
            }

            if (m_bodyRemaining > 0) {
                consumeBody();
                if (m_closing)
                    return;
                if (m_bodyRemaining > 0)
                    return;   // body 还没收完，等下一次 readyRead
            }

            dispatch();

            if (m_closing)
                return;
            if (m_download)
                return;   // 下载走异步：等 bytesWritten 把数据推完再回来
            if (!m_keepAlive) {
                closeConnection();
                return;
            }

            resetRequest();
            if (m_inBuf.isEmpty())
                return;   // 没有流水线请求了
        }
    }

    /// 解析一个请求头。返回 false 表示"数据还不够"或"已经回了错误并准备断开"。
    bool parseHead()
    {
        const int end = m_inBuf.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_inBuf.size() > kMaxHeaderBytes) {
                failAndClose(431, QStringLiteral("请求头超过 %1 KB").arg(kMaxHeaderBytes / 1024));
            }
            return false;
        }

        const QByteArray rawHead = m_inBuf.left(end);
        m_inBuf.remove(0, end + 4);

        m_head = parseRequestHead(rawHead);
        if (!m_head.valid) {
            failAndClose(400, QStringLiteral("无法解析请求行"));
            return false;
        }

        m_keepAlive = wantsKeepAlive(m_head);
        m_headerDone = true;

        const QString lengthText = m_head.header(QStringLiteral("content-length"));
        qint64 length = lengthText.isEmpty() ? 0 : lengthText.toLongLong();
        if (length < 0)
            length = 0;

        const bool hasBody = m_head.method == QLatin1String("POST")
                             || m_head.method == QLatin1String("PUT");
        if (!hasBody)
            return true;

        if (m_head.path == QLatin1String("/api/upload")) {
            if (length <= 0) {
                failAndClose(400, QStringLiteral("上传内容为空"));
                return false;
            }
            if (length > ShareStore::kMaxUpload) {
                failAndClose(413, QStringLiteral("单次上传最多 %1").arg(humanSize(ShareStore::kMaxUpload)));
                return false;
            }
            QString error;
            if (!beginUpload(&error)) {
                failAndClose(400, error);
                return false;
            }
        } else if (length > kMaxJsonBodyBytes) {
            failAndClose(413, QStringLiteral("请求体过大"));
            return false;
        }

        m_bodyRemaining = length;
        return true;
    }

    /// 把缓冲区里的 body 字节消耗掉：上传直接写盘，其它方法累计到 m_bodyBuf。
    void consumeBody()
    {
        while (m_bodyRemaining > 0 && !m_inBuf.isEmpty()) {
            const qint64 take = qMin<qint64>(static_cast<qint64>(m_inBuf.size()), m_bodyRemaining);
            const QByteArray chunk = m_inBuf.left(static_cast<int>(take));
            m_inBuf.remove(0, static_cast<int>(take));
            m_bodyRemaining -= take;

            if (m_upload) {
                if (m_upload->write(chunk) != static_cast<qint64>(chunk.size())) {
                    const QString reason = m_upload->errorString();
                    abortUpload();
                    failAndClose(500, QStringLiteral("写入文件失败：%1").arg(reason));
                    return;
                }
            } else {
                m_bodyBuf += chunk;
            }
        }
    }

    void resetRequest()
    {
        m_head = RequestHead();
        m_headerDone = false;
        m_bodyRemaining = 0;
        m_bodyBuf.clear();
        // m_upload 在 finishUpload()/abortUpload() 里已释放，不移交到这里
    }

    void continueLoop()
    {
        if (!m_keepAlive) {
            closeConnection();
            return;
        }
        resetRequest();
        processBuffer();
    }

    void closeConnection()
    {
        m_closing = true;
        // disconnectFromHost() 会先把已缓冲的数据写完再断；abort() 会把响应截断，
        // 浏览器看到的就是"下载失败"。
        m_socket->disconnectFromHost();
    }

    // ---------------------------------------------------------------- 路由

    void dispatch()
    {
        const QString &path = m_head.path;
        const QString &method = m_head.method;

        if (method == QLatin1String("GET") || method == QLatin1String("HEAD")) {
            const bool headOnly = method == QLatin1String("HEAD");
            if (path == QLatin1String("/") || path == QLatin1String("/index.html")) {
                sendResponse(200, "text/html; charset=utf-8", QByteArray(indexHtmlData()), headOnly);
                return;
            }
            if (path == QLatin1String("/api/state")) {
                sendResponse(200, "application/json; charset=utf-8", stateJson(), headOnly);
                return;
            }
            if (path == QLatin1String("/favicon.ico")) {
                sendResponse(204, "image/x-icon", QByteArray(), headOnly);
                return;
            }
            if (path.startsWith(QLatin1String("/f/"))) {
                serveFile(path.mid(3), headOnly);
                return;
            }
            if (path.startsWith(QLatin1String("/t/"))) {
                serveText(path.mid(3), headOnly);
                return;
            }
            respondError(404, QStringLiteral("没有这个地址：%1").arg(path));
            return;
        }

        if (method == QLatin1String("POST")) {
            if (path == QLatin1String("/api/text")) {
                handleTextPost();
                return;
            }
            if (path == QLatin1String("/api/upload")) {
                finishUpload();
                return;
            }
            respondError(404, QStringLiteral("没有这个接口：%1").arg(path));
            return;
        }

        if (method == QLatin1String("DELETE")) {
            handleDelete(path);
            return;
        }

        respondError(405, QStringLiteral("不支持的请求方法：%1").arg(method));
    }

    void serveFile(const QString &id, bool headOnly)
    {
        const FileEntry *entry = ShareStore::instance().findFile(id);
        if (!entry) {
            respondError(404, QStringLiteral("文件已不在共享列表里（可能被移除）"));
            return;
        }

        auto *file = new QFile(entry->path, this);
        if (!file->open(QIODevice::ReadOnly)) {
            const QString reason = file->errorString();
            file->deleteLater();
            // 源文件被移动/删除/占用都会走到这里。给可读原因，不要只回 404。
            respondError(404, QStringLiteral("文件已无法读取：%1").arg(reason));
            return;
        }

        const qint64 size = file->size();
        QByteArray head;
        head += "HTTP/1.1 200 OK\r\n";
        head += "Content-Type: " + mimeTypeForName(entry->name) + "\r\n";
        head += "Content-Length: " + QByteArray::number(size) + "\r\n";
        head += contentDispositionAttachment(entry->name) + "\r\n";
        // 不做断点续传：一次整文件传输，别让下载器反复发 Range 试探。
        head += "Accept-Ranges: none\r\n";
        head += "Cache-Control: no-store\r\n";
        head += "Connection: " + QByteArray(m_keepAlive ? "keep-alive" : "close") + "\r\n";
        head += "\r\n";
        m_socket->write(head);

        if (headOnly) {
            file->close();
            file->deleteLater();
            return;
        }

        m_download = file;
        m_downloadRemaining = size;
        pumpDownload();
    }

    void serveText(const QString &id, bool headOnly)
    {
        const TextEntry *entry = ShareStore::instance().findText(id);
        if (!entry) {
            respondError(404, QStringLiteral("文本已不在共享列表里"));
            return;
        }
        sendResponse(200, "text/plain; charset=utf-8", entry->text.toUtf8(), headOnly);
    }

    void handleTextPost()
    {
        const QJsonDocument doc = QJsonDocument::fromJson(m_bodyBuf);
        if (!doc.isObject()) {
            respondError(400, QStringLiteral("请求体必须是 JSON 对象"));
            return;
        }
        QString error;
        const QString id = ShareStore::instance().addText(
            doc.object().value(QStringLiteral("text")).toString(), peerIp(), &error);
        if (id.isEmpty()) {
            respondError(400, error);
            return;
        }
        QJsonObject reply;
        reply.insert(QStringLiteral("ok"), true);
        reply.insert(QStringLiteral("id"), id);
        sendJson(reply);
        qInfo().noquote() << "[share] 收到文本" << id << "来自" << peerIp();
    }

    void handleDelete(const QString &path)
    {
        const QString filePrefix = QStringLiteral("/api/file/");
        const QString textPrefix = QStringLiteral("/api/text/");

        if (path.startsWith(filePrefix)) {
            const QString id = path.mid(filePrefix.size());
            if (!ShareStore::instance().removeFile(id)) {
                respondError(404, QStringLiteral("文件已不在共享列表里"));
                return;
            }
            qInfo().noquote() << "[share] 移除共享文件" << id << "（请求来自" << peerIp() << "）";
        } else if (path.startsWith(textPrefix)) {
            const QString id = path.mid(textPrefix.size());
            if (!ShareStore::instance().removeText(id)) {
                respondError(404, QStringLiteral("文本已不在共享列表里"));
                return;
            }
        } else {
            respondError(404, QStringLiteral("没有这个接口：%1").arg(path));
            return;
        }

        QJsonObject reply;
        reply.insert(QStringLiteral("ok"), true);
        sendJson(reply);
    }

    // ---------------------------------------------------------------- 上传

    /// 打开落盘文件。文件名来自 X-File-Name（percent-encoded，故可安全放 header）。
    bool beginUpload(QString *error)
    {
        QString name = percentDecode(m_head.header(QStringLiteral("x-file-name")).toUtf8());
        if (name.isEmpty()) {
            *error = QStringLiteral("缺少 X-File-Name 头");
            return false;
        }

        // 只取最后一段，防止 "../../xxx" 把文件写到上传目录之外。
        name = QFileInfo(name).fileName();

        // Windows 的非法字符在其他平台也一并替换：同一份共享在两端命名行为不一致
        // 会让人以为"文件丢了"。控制字符顺手也换成下划线。
        const QString invalid = QStringLiteral("\\/:*?\"<>|");
        for (int i = 0; i < name.size(); ++i) {
            const QChar ch = name.at(i);
            if (ch.unicode() < 0x20 || invalid.contains(ch))
                name[i] = QLatin1Char('_');
        }
        name = name.trimmed();
        if (name.isEmpty())
            name = QStringLiteral("upload.bin");
        if (name.size() > 120)
            name = name.right(120);   // 各文件系统普遍限制 255 字节，留足扩展名空间

        const QString dir = ShareStore::instance().uploadDir();
        if (dir.isEmpty()) {
            *error = QStringLiteral("无法创建上传目录（检查用户数据目录的写权限）");
            return false;
        }

        // 同名不覆盖：桌面端可能已经共享了一个同名文件，覆盖会让两边看到的内容对不上。
        QString target = dir + QLatin1Char('/') + name;
        if (QFileInfo::exists(target)) {
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            const QString base = dot > 0 ? name.left(dot) : name;
            const QString suffix = dot > 0 ? name.mid(dot) : QString();
            for (int i = 1; i < 1000 && QFileInfo::exists(target); ++i)
                target = QStringLiteral("%1/%2 (%3)%4").arg(dir, base).arg(i).arg(suffix);
        }

        m_upload = new QSaveFile(target, this);
        if (!m_upload->open(QIODevice::WriteOnly)) {
            const QString reason = m_upload->errorString();
            m_upload->deleteLater();
            m_upload = nullptr;
            *error = QStringLiteral("无法写入上传目录：%1").arg(reason);
            return false;
        }
        m_uploadPath = target;
        return true;
    }

    void finishUpload()
    {
        if (!m_upload) {
            respondError(400, QStringLiteral("没有正在进行的上传"));
            return;
        }
        QSaveFile *file = m_upload;
        m_upload = nullptr;

        // commit() 才是真正落盘的那一步：它在失败时保留旧目标文件，不会留半截文件。
        if (!file->commit()) {
            const QString reason = file->errorString();
            file->deleteLater();
            QFile::remove(m_uploadPath);
            m_uploadPath.clear();
            respondError(500, QStringLiteral("保存文件失败：%1").arg(reason));
            return;
        }
        file->deleteLater();

        QString error;
        if (!ShareStore::instance().addFile(m_uploadPath, &error)) {
            // 没进共享列表就别占着磁盘
            QFile::remove(m_uploadPath);
            m_uploadPath.clear();
            respondError(400, error);
            return;
        }
        qInfo().noquote() << "[share] 已接收上传文件" << m_uploadPath << "来自" << peerIp();
        m_uploadPath.clear();

        QJsonObject reply;
        reply.insert(QStringLiteral("ok"), true);
        sendJson(reply);
    }

    void abortUpload()
    {
        if (!m_upload)
            return;
        m_upload->cancelWriting();
        m_upload->deleteLater();
        m_upload = nullptr;
        QFile::remove(m_uploadPath);
        m_uploadPath.clear();
    }

    // ---------------------------------------------------------------- 下载

    void onBytesWritten()
    {
        if (m_download)
            pumpDownload();
    }

    /// 按高水位持续把文件推进 socket 缓冲。全部推完后恢复主循环。
    void pumpDownload()
    {
        while (m_download && m_downloadRemaining > 0
               && m_socket->bytesToWrite() < kSocketHighWater) {
            const qint64 want = qMin<qint64>(kDownloadChunk, m_downloadRemaining);
            const QByteArray chunk = m_download->read(want);
            if (chunk.isEmpty()) {
                // 读失败常见于文件被别的程序截断/替换。不能假装传完 —— 直接断开，
                // 浏览器会显示下载不完整，比默默给一个短文件诚实。
                qWarning().noquote() << "[share] 下载中断：读取失败" << m_download->fileName();
                m_download->deleteLater();
                m_download = nullptr;
                m_downloadRemaining = 0;
                closeConnection();
                return;
            }
            m_socket->write(chunk);
            m_downloadRemaining -= chunk.size();
        }

        if (m_download && m_downloadRemaining == 0) {
            m_download->close();
            m_download->deleteLater();
            m_download = nullptr;
            continueLoop();
        }
    }

    // ---------------------------------------------------------------- 响应

    void sendResponse(int code, const QByteArray &contentType, const QByteArray &body,
                      bool headOnly = false)
    {
        QByteArray head;
        head += "HTTP/1.1 " + QByteArray::number(code) + ' '
                + httpStatusText(code).toLatin1() + "\r\n";
        head += "Content-Type: " + contentType + "\r\n";
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        head += "Cache-Control: no-store\r\n";
        head += "X-Content-Type-Options: nosniff\r\n";
        head += "Connection: " + QByteArray(m_keepAlive ? "keep-alive" : "close") + "\r\n";
        head += "\r\n";
        m_socket->write(head);
        if (!headOnly && !body.isEmpty())
            m_socket->write(body);
    }

    void sendJson(const QJsonObject &object, int code = 200, bool headOnly = false)
    {
        sendResponse(code, "application/json; charset=utf-8",
                     QJsonDocument(object).toJson(QJsonDocument::Compact), headOnly);
    }

    void respondError(int code, const QString &message)
    {
        QJsonObject object;
        object.insert(QStringLiteral("error"), message);
        sendJson(object, code);
        qInfo().noquote() << "[share] 请求失败" << code << message;
    }

    /// 回错误并断开。用于「在读 body 之前就判定这个请求不可继续」的场合
    /// （头过大、超限、缺文件名）：此时继续读一个几 GB 的 body 毫无意义。
    void failAndClose(int code, const QString &message)
    {
        m_keepAlive = false;
        respondError(code, message);
        closeConnection();
    }

    // ---------------------------------------------------------------- 数据

    QByteArray stateJson()
    {
        const ShareStore &store = ShareStore::instance();

        QJsonArray files;
        const QList<FileEntry> &fileList = store.files();
        for (const FileEntry &entry : fileList) {
            QJsonObject item;
            item.insert(QStringLiteral("id"), entry.id);
            item.insert(QStringLiteral("name"), entry.name);
            // QJsonValue 没有 64 位整数，用 double 存（2 GB 内精确）；
            // 同时给一份格式化好的文本，前端不用再写一遍单位换算，两边显示永远一致。
            item.insert(QStringLiteral("size"), static_cast<double>(entry.size));
            item.insert(QStringLiteral("sizeText"), humanSize(entry.size));
            item.insert(QStringLiteral("time"),
                        entry.addedAt.toString(QStringLiteral("MM-dd HH:mm")));
            item.insert(QStringLiteral("exists"), QFileInfo::exists(entry.path));
            files.append(item);
        }

        QJsonArray texts;
        const QList<TextEntry> &textList = store.texts();
        for (const TextEntry &entry : textList) {
            QJsonObject item;
            item.insert(QStringLiteral("id"), entry.id);
            item.insert(QStringLiteral("text"), entry.text);
            item.insert(QStringLiteral("from"), entry.from);
            item.insert(QStringLiteral("time"),
                        entry.createdAt.toString(QStringLiteral("MM-dd HH:mm")));
            texts.append(item);
        }

        QJsonArray urls;
        // 用 localPort() 而不是去问服务器：服务端 accept 出来的连接，本端端口就是
        // 监听端口（QTcpSocket 没有 serverPort() 这个成员）。
        const QList<LocalAddress> addresses = localAddresses(m_socket->localPort());
        for (const LocalAddress &address : addresses)
            urls.append(address.url);

        QJsonObject root;
        root.insert(QStringLiteral("files"), files);
        root.insert(QStringLiteral("texts"), texts);
        root.insert(QStringLiteral("urls"), urls);
        root.insert(QStringLiteral("maxUpload"), static_cast<double>(ShareStore::kMaxUpload));
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    QString peerIp() const
    {
        QString ip = m_socket->peerAddress().toString();
        // 双栈 socket 上 IPv4 对端会显示成 ::ffff:192.168.1.9，去掉映射前缀才认得出
        if (ip.startsWith(QLatin1String("::ffff:")))
            ip = ip.mid(7);
        return ip;
    }

    QTcpSocket *m_socket = nullptr;
    QByteArray m_inBuf;        ///< 尚未处理的原始字节（含 body）
    RequestHead m_head;
    bool m_headerDone = false; ///< 当前请求的头已解析
    bool m_keepAlive = true;   ///< 当前请求处理完后是否复用连接
    bool m_closing = false;    ///< 已决定断开，主循环要停
    qint64 m_bodyRemaining = 0;
    QByteArray m_bodyBuf;      ///< 非上传请求的 body
    QSaveFile *m_upload = nullptr;
    QString m_uploadPath;
    QFile *m_download = nullptr;
    qint64 m_downloadRemaining = 0;
};

} // namespace

// ------------------------------------------------------------------ 服务

ShareServer &ShareServer::instance()
{
    // 同 Store：不析构。socket 与连接对象交给操作系统回收，避免退出阶段
    // 在插件模块的静态析构里跑 Qt 网络代码。
    static ShareServer *server = new ShareServer;
    return *server;
}

ShareServer::ShareServer(QObject *parent)
    : QObject(parent)
{
}

bool ShareServer::isRunning() const
{
    return m_server && m_server->isListening();
}

bool ShareServer::start(quint16 preferredPort, QString *error)
{
    if (isRunning()) {
        if (m_port == preferredPort)
            return true;
        stop();
    }

    if (!m_server) {
        m_server = new QTcpServer(this);
        connect(m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server->nextPendingConnection())
                new HttpConnection(socket, this);   // 由 socket 的 disconnected 负责自毁
        });
    }

    // 端口顺延：5421 被别的程序占了（或者上次没退干净）不该让整个工具不可用。
    constexpr int kPortTries = 20;
    for (int offset = 0; offset < kPortTries; ++offset) {
        const int candidate = static_cast<int>(preferredPort) + offset;
        if (candidate > 65535)
            break;
        if (m_server->listen(QHostAddress::AnyIPv4, static_cast<quint16>(candidate))) {
            m_port = static_cast<quint16>(candidate);
            if (m_port != preferredPort)
                qInfo().noquote() << "[share] 端口" << preferredPort << "不可用，改用" << m_port;
            qInfo().noquote() << "[share] 共享服务已启动：" << primaryUrl();
            emit statusChanged();
            return true;
        }
    }

    if (error) {
        *error = QStringLiteral("无法监听端口 %1–%2：%3")
                     .arg(preferredPort)
                     .arg(preferredPort + kPortTries - 1)
                     .arg(m_server->errorString());
    }
    return false;
}

void ShareServer::stop()
{
    if (!m_server || !m_server->isListening())
        return;
    m_server->close();
    m_port = 0;
    emit statusChanged();
}

QList<LocalAddress> ShareServer::addresses() const
{
    if (!isRunning())
        return {};
    return localAddresses(m_port);
}

QString ShareServer::primaryUrl() const
{
    if (!isRunning())
        return QString();
    const QList<LocalAddress> list = localAddresses(m_port);
    if (list.isEmpty()) {
        // 完全没网卡（或只有回环）时退回本机地址：至少能验证服务是活的，
        // 而不是显示一片空白让人以为启动失败。
        return QStringLiteral("http://127.0.0.1:%1").arg(m_port);
    }
    return list.first().url;
}

} // namespace share

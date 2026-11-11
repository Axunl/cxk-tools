#include "UpdateChecker.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

namespace {

// 更新源固定指向本仓库 release 上的 latest.json。
//
// 为什么不用 api.github.com/repos/.../releases/latest：匿名调用每小时只有 60 次，
// 而且一旦被限流，用户看到的是「检查更新失败」，很难解释。releases/latest/download/
// 是 CDN 重定向，直接拿文件，没有配额问题。
constexpr auto kFeedUrl = "https://github.com/Axunl/cxk-tools/releases/latest/download/latest.json";

/// 自动检查的节流窗口。一天一次足够，也不至于让用户觉得程序老在联网。
constexpr int kCheckIntervalHours = 24;
/// 启动后多久开始静默检查：先让窗口画出来，别和首帧抢时间。
constexpr int kStartupDelayMs = 3000;
/// 清单只有一两 KB，但一次请求要经历 DNS + TCP + TLS 三轮往返，慢网/弱信号下
/// 10 秒确实不够（会卡在握手上），这里放宽到 12 秒。
constexpr int kFeedTimeoutMs = 12000;
/// 清单最多试 2 次。只在"网络类"错误上重试：404、清单损坏是服务端状态，
/// 再试一百次也一样，立刻把原因告诉用户才有用。
constexpr int kFeedAttempts = 2;
/// 下载的空闲超时。注意 Qt 的 setTransferTimeout 是「多久没有新数据」就掐断
/// （实测：1 秒超时也拦不住一个持续 3.5 秒、数据一直在流的下载），不是总时长上限。
/// 三四十兆的包在慢网下跑十几分钟没问题；但运营商/代理常在二三十秒的停顿后才继续
/// 吐数据，30 秒太紧，放宽到 45 秒。
constexpr int kDownloadTimeoutMs = 45000;
/// 上一轮一个字节都没收到时（多半是根本连不上：被墙、代理挂了、没网），
/// 用短超时快速失败。否则"网络不通"要让用户干等 5 × 45 秒才看到一句超时。
constexpr int kStalledTimeoutMs = 15000;
/// 断流后最多重连几次（含第一次）。每次重连都从已下载的位置续传，不是从头再来。
constexpr int kDownloadAttempts = 5;
/// 第 n 次重连前等 n × 1.2 秒：给代理/路由一点恢复时间，也避免疯狂重试惹恼服务器。
constexpr int kRetryBackoffMs = 1200;
/// 续传前要把已有 .part 读一遍算进 SHA256，按块读，别一次啃 30MB 到内存。
constexpr qint64 kHashChunkSize = 1 << 20;

QString userAgent()
{
    return QStringLiteral("cxk-tools/%1").arg(QCoreApplication::applicationVersion());
}

/// 下载落盘目录：临时目录下的固定子目录，重装/退出后由系统清理。
QString downloadDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
           + QStringLiteral("/cxk-tools-update");
}

/// 这些错误属于"过一会儿再试可能就好了"，值得自动重连：
/// 超时（Qt 报 OperationCanceledError）、对端断连、DNS 抖动、网络切换。
bool isRetryableError(QNetworkReply::NetworkError code)
{
    switch (code) {
    case QNetworkReply::OperationCanceledError:   // 传输超时
    case QNetworkReply::TimeoutError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::UnknownServerError:
        return true;
    default:
        // 4xx（地址没了、没权限）、证书错误、内容不合法：重试只是浪费用户时间
        return false;
    }
}

/// 5xx 与 408/429 是服务端临时状态，其余 4xx 是确定性失败。
bool isRetryableStatus(int status)
{
    return status == 408 || status == 429 || (status >= 500 && status <= 599);
}

/// 第 attempt 次失败后等多少毫秒再重试。
int retryDelayMs(int attempt)
{
    return kRetryBackoffMs * qMax(1, attempt);
}

/// 给用户看的体积文案。别直接抛原始字节数（"已下载 5242880" 没人想读）。
QString bytesText(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(qMax<qint64>(1, bytes / 1024));
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
    // 只给"更新"这一个 manager 套系统代理（Windows 读 WinINET 设置，含 PAC）。
    //
    // 为什么不全局 QNetworkProxyFactory::setUseSystemConfiguration：那会连带改掉所有插件
    // 的网络行为，超出「修更新」的范围。而更新恰恰必须和浏览器保持一致——用户靠代理
    // 能打开 GitHub，程序却直连，用户看到的就是"永远下不动"，还查不出原因。
    // 没配代理时这里拿到的是 NoProxy，等于什么都不做。
    const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(
        QNetworkProxyQuery(QUrl(QString::fromLatin1(kFeedUrl))));
    if (!proxies.isEmpty() && proxies.first().type() != QNetworkProxy::NoProxy) {
        m_nam.setProxy(proxies.first());
        qInfo() << "UpdateChecker: 使用系统代理" << proxies.first().hostName()
                << proxies.first().port();
    }
}

UpdateChecker::~UpdateChecker()
{
    // 析构时还挂着请求说明正在退出。先断开信号再 abort：abort() 会让 finished 同步触发，
    // 那时本对象正在析构，回调里任何 emit 都可能打到一个半死的对话框上。
    // .part 故意留在磁盘上——下次启动点「下载」能接着续传。
    for (QNetworkReply *reply : {m_feedReply, m_downloadReply}) {
        if (!reply)
            continue;
        reply->disconnect(this);
        reply->abort();
    }
    m_feedReply = nullptr;
    m_downloadReply = nullptr;
    if (m_downloadFile.isOpen())
        m_downloadFile.close();
}

QUrl UpdateChecker::feedUrl()
{
    // 覆盖口只用于本地验证（指向 file:// 的假清单）与将来可能的内网镜像
    const QByteArray override = qgetenv("XTOOLS_UPDATE_FEED");
    if (!override.isEmpty())
        return QUrl::fromEncoded(override);
    return QUrl(QString::fromLatin1(kFeedUrl));
}

void UpdateChecker::scheduleStartupCheck()
{
    Settings &settings = Settings::instance();

    if (!settings.autoCheckUpdate()) {
        qInfo() << "UpdateChecker: 自动检查已关闭，跳过启动检查";
        return;
    }

    const QDateTime last = settings.lastUpdateCheck();
    if (last.isValid()) {
        const qint64 elapsed = last.secsTo(QDateTime::currentDateTimeUtc());
        if (elapsed >= 0 && elapsed < qint64(kCheckIntervalHours) * 3600) {
            qInfo() << "UpdateChecker: 距上次检查不足 24 小时，跳过";
            return;
        }
    }

    QTimer::singleShot(kStartupDelayMs, this, [this] {
        m_silent = true;
        qInfo() << "UpdateChecker: 启动静默检查" << feedUrl().toString();
        requestFeed();
    });
}

void UpdateChecker::check()
{
    m_silent = false;

    // 换一代代号：正在飞的请求与已经排队的重试回调都会认出自己过期而退出。
    // 用户连点两下「检查更新」不该压两条请求，也不该让旧结果覆盖新结果。
    ++m_feedGeneration;
    if (m_feedReply) {
        m_feedReply->abort();
        m_feedReply = nullptr;
    }
    requestFeed();
}

void UpdateChecker::requestFeed()
{
    emit checking();
    m_feedAttempt = 1;
    fireFeedRequest(m_feedGeneration);
}

void UpdateChecker::fireFeedRequest(int generation)
{
    QNetworkRequest request(feedUrl());
    request.setTransferTimeout(kFeedTimeoutMs);
    m_currentTimeoutMs = kFeedTimeoutMs;
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    // CDN 会缓存 latest.json；发新版后立刻检查却拿到旧清单是最难查的一类问题
    request.setRawHeader("Cache-Control", "no-cache");

    m_feedReply = m_nam.get(request);
    // 必须把 reply 抓进 lambda：用户再点一次「检查更新」时，新请求会覆盖 m_feedReply，
    // 而旧 reply 的 finished 是异步来的——从成员里读会读成新 reply，然后把它删掉。
    QNetworkReply *reply = m_feedReply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        if (m_feedReply == reply)
            m_feedReply = nullptr;
        handleFeedFinished(reply, generation);
        reply->deleteLater();
    });
}

void UpdateChecker::handleFeedFinished(QNetworkReply *reply, int generation)
{
    // 用户中途又点了一次检查：这次结果已经过期，静默丢掉
    if (generation != m_feedGeneration)
        return;

    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError code = reply->error();
        const QString detail = describeNetworkError(code, reply->errorString());
        qWarning() << "UpdateChecker: 清单请求失败" << detail << "status" << status;

        // 网络类错误先自己重试：静默检查只写日志，用户根本没机会手动重试
        const bool transient = isRetryableError(code) || isRetryableStatus(status);
        if (transient && m_feedAttempt < kFeedAttempts) {
            ++m_feedAttempt;
            const int delay = retryDelayMs(m_feedAttempt);
            qInfo() << "UpdateChecker:" << delay << "ms 后重试清单请求";
            QTimer::singleShot(delay, this, [this, generation] {
                if (generation != m_feedGeneration)
                    return;
                fireFeedRequest(generation);
            });
            return;
        }

        // 404 多半是「仓库还没发过版」，说人话比抛错误码有用
        if (status == 404)
            emit failed(QStringLiteral("更新源还没有发布过版本（HTTP 404）"));
        else
            emit failed(QStringLiteral("检查更新失败：%1").arg(detail));
        return;
    }

    update::Info info;
    QString parseError;
    if (!update::parseLatestJson(reply->readAll(), &info, &parseError)) {
        emit failed(QStringLiteral("更新清单无法解析：%1").arg(parseError));
        return;
    }

    // 只有真正拿到并解析成功才算「检查过」，否则网络抖动会让节流把用户锁 24 小时
    Settings::instance().setLastUpdateCheck(QDateTime::currentDateTimeUtc());

    const QString current = QCoreApplication::applicationVersion();
    qInfo() << "UpdateChecker: 远端版本" << info.version << "本机版本" << current;

    if (update::compareVersions(info.version, current) > 0) {
        // 静默检查时尊重用户点过的「稍后」：同一个版本不再反复骚扰；
        // 手动检查仍然提示，否则用户会觉得按钮坏了。
        if (m_silent && Settings::instance().skippedVersion() == info.version) {
            qInfo() << "UpdateChecker: 用户已跳过" << info.version << "，静默检查不再提示";
            return;
        }
        emit updateAvailable(info);
        return;
    }

    emit upToDate(current);
}

void UpdateChecker::download(const update::Asset &asset)
{
    if (m_downloadReply || m_retryPending) {
        emit downloadFailed(QStringLiteral("上一个下载还没结束"));
        return;
    }
    if (asset.url.isEmpty()) {
        emit downloadFailed(QStringLiteral("更新清单里的下载地址为空"));
        return;
    }

    const QString dir = downloadDir();
    if (!QDir().mkpath(dir)) {
        emit downloadFailed(QStringLiteral("无法创建下载目录：%1").arg(QDir::toNativeSeparators(dir)));
        return;
    }

    // 只取文件名，避免清单里带上 "../" 之类的东西把文件写到别处
    const QString name = QFileInfo(asset.name).fileName();
    m_downloadAsset = asset;
    m_targetPath = dir + QLatin1Char('/') + name;
    m_partPath = m_targetPath + QStringLiteral(".part");
    m_received = 0;
    m_total = asset.size;
    m_attempt = 0;
    m_retryPending = false;
    m_useStalledTimeout = false;
    m_localError.clear();
    m_hash.reset();

    // 上次失败留下的半成品：能续就续，不能续（比目标还大 / 读不出来）就丢掉重下。
    // 这一步是"更新老是失败"体验里最值钱的地方——慢网下一次断流不用重下几十兆。
    const QFileInfo partInfo(m_partPath);
    if (partInfo.exists()) {
        const bool usable = partInfo.size() > 0 && (m_total <= 0 || partInfo.size() < m_total);
        if (usable) {
            QFile part(m_partPath);
            qint64 hashed = 0;
            bool ok = part.open(QIODevice::ReadOnly);
            while (ok && !part.atEnd()) {
                const QByteArray chunk = part.read(kHashChunkSize);
                if (chunk.isEmpty())
                    break;
                m_hash.addData(chunk);
                hashed += chunk.size();
            }
            if (!ok || hashed != partInfo.size()) {
                qWarning() << "UpdateChecker: 已有半成品读不出来，重新下载" << m_partPath;
                QFile::remove(m_partPath);
                m_received = 0;
                m_hash.reset();
            } else {
                m_received = hashed;
                qInfo() << "UpdateChecker: 发现上次的半成品，从" << m_received << "字节处续传";
            }
        } else {
            QFile::remove(m_partPath);
        }
    }

    qInfo() << "UpdateChecker: 开始下载" << name << "from" << asset.url.toString()
            << "offset" << m_received;
    startAttempt();
}

void UpdateChecker::startAttempt()
{
    ++m_attempt;
    m_retryPending = false;
    m_rangeChecked = false;
    m_attemptBase = m_received;

    // 有断点就用 Append 接着写，没有就新建。不能用 ReadWrite 覆盖写：
    // 那样一续传就把前面下好的数据抹了。
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    mode |= (m_received > 0) ? QIODevice::Append : QIODevice::Truncate;

    m_downloadFile.setFileName(m_partPath);
    if (!m_downloadFile.open(mode)) {
        // 写不了盘（没空间 / 被安全软件占着）重试也没用，直接报
        failDownload(QStringLiteral("无法写入临时文件：%1（%2）")
                         .arg(QDir::toNativeSeparators(m_partPath), m_downloadFile.errorString()),
                     false);
        return;
    }

    const qint64 offset = m_received;

    QNetworkRequest request(m_downloadAsset.url);
    // 已有进展就按正常超时等（慢网要时间）；上一轮零字节说明连通性有问题，快速失败
    m_currentTimeoutMs = m_useStalledTimeout ? kStalledTimeoutMs : kDownloadTimeoutMs;
    request.setTransferTimeout(m_currentTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    if (offset > 0) {
        // 断点续传：服务器若支持会回 206 并只给剩下的部分
        request.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + "-");
        qInfo() << "UpdateChecker: 第" << m_attempt << "次尝试，Range 从" << offset << "开始";
    }

    m_downloadReply = m_nam.get(request);
    QNetworkReply *reply = m_downloadReply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        // 只认当前这一个 reply：重连后旧 reply 的迟到事件要直接丢掉
        if (m_downloadReply != reply)
            return;

        // 只判断一次：服务器若不认 Range 会回 200 + 整包，此时必须把已写的数据作废，
        // 否则续传出来的文件前半段是旧的、后半段是新的，校验永远不过。
        if (!m_rangeChecked) {
            m_rangeChecked = true;
            const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            if (status.isValid() && status.toInt() == 200 && m_received > 0) {
                qWarning() << "UpdateChecker: 服务器不支持断点续传（HTTP 200），从头重写";
                m_downloadFile.resize(0);
                m_hash.reset();
                m_received = 0;
                m_attemptBase = 0;
                emit downloadProgress(0, m_total);
            }
        }

        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;

        // 先确认写得进去再进 hash：写一半失败的话，hash 里留着磁盘上没有的数据，
        // 后面续传算出来的校验值就永远对不上。
        if (m_downloadFile.write(chunk) != chunk.size()) {
            m_localError = QStringLiteral("写入安装包失败：%1").arg(m_downloadFile.errorString());
            reply->abort();
            return;
        }
        m_hash.addData(chunk);
        m_received += chunk.size();

        if (m_total <= 0)
            m_total = m_attemptBase
                      + reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        emit downloadProgress(m_received, m_total);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_downloadReply == reply)
            m_downloadReply = nullptr;
        handleDownloadFinished(reply);
        reply->deleteLater();
    });
}

void UpdateChecker::handleDownloadFinished(QNetworkReply *reply)
{
    // 收尾可能还有没读的尾巴（readyRead 不保证在 finished 前再触发一次）
    const QByteArray tail = reply->readAll();
    if (!tail.isEmpty() && m_localError.isEmpty()) {
        if (m_downloadFile.write(tail) == tail.size()) {
            m_hash.addData(tail);
            m_received += tail.size();
        } else {
            m_localError = QStringLiteral("写入安装包失败：%1").arg(m_downloadFile.errorString());
        }
    }

    const QNetworkReply::NetworkError code = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString raw = reply->errorString();

    m_downloadFile.close();

    // 本地故障（写盘）优先报：它的错误码会被 abort() 掩盖成 Operation canceled
    if (!m_localError.isEmpty()) {
        failDownload(m_localError, false);
        return;
    }

    if (code != QNetworkReply::NoError) {
        const QString reason = describeNetworkError(code, raw);
        const bool transient = isRetryableError(code) || isRetryableStatus(status);
        qWarning() << "UpdateChecker: 第" << m_attempt << "次尝试失败，" << reason
                   << "已下载" << m_received << "/" << m_total << "raw" << raw;
        // 本轮一个字节都没收到 -> 下一轮改用短超时，别让"没网"变成五分钟的等待
        m_useStalledTimeout = (m_received <= m_attemptBase);
        if (transient && m_attempt < kDownloadAttempts) {
            scheduleRetry(reason);
            return;
        }
        // 保留半成品：用户下次点「下载」直接从断点接着来
        failDownload(reason, true);
        return;
    }

    // 连接正常结束但字节数不够（对端提前闭嘴）：续传还能救
    if (m_downloadAsset.size > 0 && m_received < m_downloadAsset.size) {
        const QString reason = QStringLiteral("服务器提前结束了传输（%1/%2 字节）")
                                   .arg(m_received)
                                   .arg(m_downloadAsset.size);
        qWarning() << "UpdateChecker:" << reason;
        if (m_attempt < kDownloadAttempts) {
            scheduleRetry(reason);
            return;
        }
        failDownload(reason, true);
        return;
    }

    verifyDownload();
}

void UpdateChecker::scheduleRetry(const QString &reason)
{
    m_retryPending = true;
    const int delay = retryDelayMs(m_attempt);
    emit downloadRetrying(m_attempt, kDownloadAttempts, reason);
    qInfo() << "UpdateChecker:" << delay << "ms 后自动重连续传";

    // 以 this 作为上下文：控制器被销毁时这个回调会一起取消，不会打到野指针上
    QTimer::singleShot(delay, this, [this] { startAttempt(); });
}

void UpdateChecker::verifyDownload()
{
    // 清单给了校验值就必须对上：下到半截、被运营商插广告、代理返回错误页，
    // 单靠"文件能打开"是发现不了的。
    if (!m_downloadAsset.sha256.isEmpty()) {
        const QString actual = QString::fromLatin1(m_hash.result().toHex());
        if (actual.compare(m_downloadAsset.sha256, Qt::CaseInsensitive) != 0) {
            qWarning() << "UpdateChecker: SHA256 不匹配，期望" << m_downloadAsset.sha256
                       << "实际" << actual;
            failDownload(QStringLiteral("安装包校验失败（SHA256 不匹配），已删除"), false);
            return;
        }
    }

    if (m_downloadAsset.size > 0 && m_received != m_downloadAsset.size) {
        failDownload(QStringLiteral("安装包不完整（%1/%2 字节），已删除")
                         .arg(m_received)
                         .arg(m_downloadAsset.size),
                     false);
        return;
    }

    // 校验通过才把 .part 改名成正式文件名：中途退出时目录里不会出现
    // 一个"看起来能装、其实是半截"的 exe。
    QFile::remove(m_targetPath);
    if (!QFile::rename(m_partPath, m_targetPath)) {
        failDownload(QStringLiteral("无法重命名下载文件：%1")
                         .arg(QDir::toNativeSeparators(m_partPath)),
                     true);
        return;
    }

    qInfo() << "UpdateChecker: 下载完成" << m_targetPath << m_received << "bytes";
    emit downloadReady(m_targetPath, m_downloadAsset);
}

void UpdateChecker::failDownload(const QString &reason, bool keepPartial)
{
    m_retryPending = false;
    if (m_downloadFile.isOpen())
        m_downloadFile.close();
    // 零字节的 .part 没有续传价值（404、地址失效时就会留下这种空文件），一并清掉
    if (!keepPartial || m_received == 0)
        QFile::remove(m_partPath);

    QString message = QStringLiteral("下载失败：%1").arg(reason);
    if (m_attempt > 1)
        message += QStringLiteral("（已自动重试 %1 次）").arg(m_attempt - 1);
    if (keepPartial && m_received > 0)
        message += QStringLiteral("；已下载 %1，重试会接着下").arg(bytesText(m_received));
    qWarning() << "UpdateChecker:" << message;

    emit downloadFailed(message);
}

QString UpdateChecker::describeNetworkError(QNetworkReply::NetworkError code, const QString &raw) const
{
    // Qt 把"传输超时"也报成 OperationCanceledError，errorString 只有
    // "Operation canceled"，直接把原文丢到界面上用户只会一脸问号。
    switch (code) {
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("网络超时（%1 秒内没有收到任何数据）")
            .arg(qMax(1, m_currentTimeoutMs) / 1000);
    case QNetworkReply::TimeoutError:
        return QStringLiteral("连接超时，网络太慢或不通");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("域名解析失败，可能没联网或被 DNS 拦截");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("服务器拒绝连接，可能被防火墙或代理拦了");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("连接被服务器提前断开");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("HTTPS 握手失败，可能是代理插了证书或系统时间不对");
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
        return QStringLiteral("网络已断开");
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyAuthenticationRequiredError:
        return QStringLiteral("代理服务器不可用，请检查系统代理设置");
    case QNetworkReply::ContentNotFoundError:
        return QStringLiteral("服务器上找不到这个文件（404）");
    case QNetworkReply::ContentAccessDenied:
        return QStringLiteral("没有权限下载这个文件（403）");
    case QNetworkReply::ProtocolUnknownError:
        return QStringLiteral("请求被网络中间层拦截（协议不识别）");
    default:
        break;
    }

    // 兜底：保留 Qt 的原文，至少日志里能查；同时给出可操作的下一步
    return raw.isEmpty() ? QStringLiteral("未知网络错误")
                         : QStringLiteral("%1（错误码 %2）").arg(raw).arg(int(code));
}

bool UpdateChecker::isInstalledBuild()
{
#if defined(Q_OS_WIN)
    // Inno Setup 会在安装目录放 unins000.exe；有它就是「安装版」，否则是便携版。
    // 用文件存在性判断而不是查注册表：便携版拷来即用，注册表里根本没有记录。
    const QString dir = QCoreApplication::applicationDirPath();
    return QFileInfo::exists(dir + QStringLiteral("/unins000.exe"));
#else
    return false;
#endif
}

bool UpdateChecker::launchInstaller(const QString &filePath, QString *error)
{
#if defined(Q_OS_WIN)
    // 静默参数与 installer/windows/cxk-tools.iss 的 [Run]/CloseApplications 配套：
    //   /SILENT            只显示进度，不弹页面
    //   /CLOSEAPPLICATIONS Restart Manager 关掉还在跑的旧实例（我们自己先退也行）
    //   /NORESTART         不要重启系统
    // 装完由 .iss 的 [Run] 段把新版本拉起来，所以这里不需要再启动一次。
    QStringList args{QStringLiteral("/SILENT"),
                     QStringLiteral("/CLOSEAPPLICATIONS"),
                     QStringLiteral("/NORESTART")};

    // 装到 %LOCALAPPDATA% 的副本是「仅为当前用户安装」（向导里选了 non-admin）。
    // 这时必须补 /CURRENTUSER，否则 Inno 按 PrivilegesRequired=admin 去提权：
    // 静默流程里 UAC 被拒绝一次，升级就悄无声息地什么都没做，用户只看到
    // 「安装包已下载、版本没变」。装在 Program Files 时保持默认（本来就需要提权）。
    const QString exeDir = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath());
    const QString localAppData = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    if (update::isPerUserInstallDir(exeDir, localAppData))
        args << QStringLiteral("/CURRENTUSER");

    if (!QProcess::startDetached(filePath, args)) {
        if (error)
            *error = QStringLiteral("无法启动安装程序：%1").arg(QDir::toNativeSeparators(filePath));
        return false;
    }
    return true;
#else
    Q_UNUSED(filePath)
    if (error)
        *error = QStringLiteral("当前平台不支持静默安装，请手动打开安装包");
    return false;
#endif
}

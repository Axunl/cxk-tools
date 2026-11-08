#include "WsServer.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {

/// 握手等待上限：客户端连上后 8 秒还没把请求头发全，就当它不是 WebSocket 客户端。
/// （浏览器 / 标准客户端都是连上立刻发，8 秒足够覆盖慢链路与手工 telnet 试错。）
constexpr int kHandshakeTimeoutMs = 8000;

/// 发完关闭帧后等对端收尾的上限，超时强制收掉会话。
constexpr int kCloseTimeoutMs = 2000;

/// 握手被拒后留给客户端读完 4xx 的时间。disconnectFromHost() 会等对端发 FIN，
/// 对端赖着不走时这个会话会一直占着 socket，所以到点强制收掉。
constexpr int kRejectGraceMs = 1500;

/// 同时在线客户端上限。调试用不到更多，加个闸门防止误暴露到公网后被刷爆。
constexpr int kMaxClients = 64;

/// 对端 "ip:port" 文案。双栈监听时 IPv4 会显示成 `::ffff:127.0.0.1`，去掉映射前缀。
QString peerText(const QTcpSocket *socket)
{
    QString address = socket->peerAddress().toString();
    if (address.startsWith(QLatin1String("::ffff:")))
        address = address.mid(7);
    return QStringLiteral("%1:%2").arg(address).arg(socket->peerPort());
}

} // namespace

/// 一个客户端的会话状态。只由 WsServer 持有，socket 的 parent 也是 WsServer，
/// 但销毁统一走 destroySession()（先断信号再 abort），避免 disconnected 绕回来重入。
struct WsServer::Session
{
    QTcpSocket *socket = nullptr;
    QString     id;
    QString     peer;
    QString     protocol;    ///< 协商出的子协议
    QString     target;      ///< 客户端请求的路径
    QString     closeReason; ///< 断开原因，用于日志

    bool open     = false;   ///< 握手完成
    bool closing  = false;   ///< 已发出关闭帧，等待 TCP 收尾
    bool rejected = false;   ///< 握手被拒（已经回过 4xx），后续数据一律丢弃

    QByteArray httpBuffer;   ///< 握手请求头（还没收全时累积）
    // 服务端收的是客户端帧，必须带掩码 —— 构造参数在这里就固定下来
    wsproto::FrameReader reader{true};

    int        fragOpcode = 0;
    QByteArray fragPayload;
};

WsServer::WsServer(QObject *parent)
    : QObject(parent)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &WsServer::onNewConnection);
}

WsServer::~WsServer()
{
    // 面板析构（切工具页 / 关独立窗口）就会走到这里：静默收掉端口与全部会话。
    //
    // blockSignals 是必须的，不只是"不想发信号"：QObject 析构时先删子对象、
    // 后断开自己的信号连接，而 m_server 是页面的子对象 —— 此刻 stopInternal
    // 若发出 clientDisconnected，页面的槽照样会被调用，而槽里的 m_log 等控件
    // 已经先一步 delete（实测退出时段错误）。挡掉全部信号才是安全做法。
    blockSignals(true);
    stopInternal(QString(), false);
}

bool WsServer::listen(const QHostAddress &address, quint16 port, const QStringList &protocols)
{
    stopInternal(QString(), false);   // 重复点「启动」时先收掉旧监听

    m_protocols = protocols;

    if (!m_server->listen(address, port)) {
        emit failure(QStringLiteral("无法监听 %1:%2：%3")
                         .arg(address.toString())
                         .arg(port)
                         .arg(m_server->errorString()));
        return false;
    }
    emit listening(m_server->serverPort());
    return true;
}

void WsServer::stop(const QString &reason)
{
    stopInternal(reason, true);
}

void WsServer::stopInternal(const QString &reason, bool announce)
{
    const QString text = reason.isEmpty() ? QStringLiteral("已停止监听") : reason;
    const QList<Session *> sessions = m_sessions.values();

    // 先尽量把关闭帧发出去，再统一收尾。这里不等待对端回应（stop 是同步操作，
    // 拖 2 秒会让「停止服务」按钮看起来卡住）；close 帧在 flush 后已经交给内核。
    for (Session *session : sessions) {
        if (!session->open || session->socket == nullptr)
            continue;
        const QByteArray frame =
            wsproto::encodeFrame(wsproto::OpClose, wsproto::closePayload(1001, text), false);
        session->socket->write(frame);
        session->socket->flush();
    }
    for (Session *session : sessions)
        destroySession(session, text, session->open);

    if (m_server->isListening())
        m_server->close();

    if (announce)
        emit stopped(text);
}

bool WsServer::isListening() const
{
    return m_server->isListening();
}

quint16 WsServer::port() const
{
    return m_server->serverPort();
}

int WsServer::clientCount() const
{
    // 只数已完成握手的会话：连接刚接入、还在等请求头时不该显示成"客户端"
    int open = 0;
    for (const Session *session : m_sessions) {
        if (session != nullptr && session->open)
            ++open;
    }
    return open;
}

QList<WsServer::Client> WsServer::clients() const
{
    QList<Client> out;
    out.reserve(m_order.size());
    for (const QString &id : m_order) {
        const Session *session = m_sessions.value(id);
        if (session == nullptr || !session->open)
            continue;
        Client client;
        client.id       = session->id;
        client.peer     = session->peer;
        client.protocol = session->protocol;
        client.target   = session->target;
        out << client;
    }
    return out;
}

bool WsServer::sendText(const QString &clientId, const QString &text)
{
    Session *session = sessionFor(clientId);
    if (session == nullptr || !session->open || session->closing)
        return false;
    return sendFrame(session, wsproto::OpText, text.toUtf8());
}

int WsServer::broadcastText(const QString &text)
{
    const QByteArray payload = text.toUtf8();
    int sent = 0;
    for (const QString &id : m_order) {
        Session *session = m_sessions.value(id);
        if (session == nullptr || !session->open || session->closing)
            continue;
        if (sendFrame(session, wsproto::OpText, payload))
            ++sent;
    }
    return sent;
}

bool WsServer::disconnectClient(const QString &clientId, int code, const QString &reason)
{
    Session *session = sessionFor(clientId);
    if (session == nullptr || !session->open)
        return false;
    emit protocolNotice(session->id,
                        QStringLiteral("服务端主动断开（%1）").arg(wsproto::closeCodeText(code)));
    sendClose(session, code, reason);
    return true;
}

// ---------------------------------------------------------------- 接入与握手

void WsServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (socket == nullptr)
            break;

        if (m_sessions.size() >= kMaxClients) {
            // 超过上限直接回 503 并关掉：不静默丢弃，客户端能看到原因
            socket->write(wsproto::buildRejectResponse(
                503, QStringLiteral("同时连接的客户端过多（上限 %1）").arg(kMaxClients)));
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }

        auto *session = new Session;
        session->id     = QStringLiteral("c%1").arg(++m_nextId);
        session->socket = socket;
        session->peer   = peerText(socket);
        m_sessions.insert(session->id, session);
        m_order << session->id;

        const QString id = session->id;
        connect(socket, &QTcpSocket::readyRead, this, [this, id] { onReadyRead(id); });
        connect(socket, &QTcpSocket::disconnected, this, [this, id] { onDisconnected(id); });
        // 错误只记一行日志：真正的收尾由 disconnected 统一处理，
        // 在这里删会话会和 onDisconnected 抢同一个对象。
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, id](QAbstractSocket::SocketError error) {
                    if (error == QAbstractSocket::RemoteHostClosedError)
                        return;
                    const Session *session = sessionFor(id);
                    if (session == nullptr)
                        return;
                    emit protocolNotice(id, QStringLiteral("连接出错：%1")
                                                .arg(session->socket->errorString()));
                });

        emit protocolNotice(id, QStringLiteral("%1 接入，等待握手").arg(session->peer));

        // 单个定时器只针对本次会话：会话已握手完成或已销毁时什么都不做
        QTimer::singleShot(kHandshakeTimeoutMs, this, [this, id] {
            Session *session = sessionFor(id);
            if (session == nullptr || session->open || session->rejected)
                return;
            const QString reason = QStringLiteral("握手超时：%1 秒内没有收到完整的握手请求")
                                       .arg(kHandshakeTimeoutMs / 1000);
            emit protocolNotice(id, reason);
            destroySession(session, reason, false);
        });
    }
}

void WsServer::onReadyRead(const QString &id)
{
    Session *session = sessionFor(id);
    if (session == nullptr || session->socket == nullptr)
        return;

    const QByteArray chunk = session->socket->readAll();
    if (chunk.isEmpty())
        return;
    if (session->rejected)
        return;   // 已经回过 4xx，剩下的数据不用解析

    if (!session->open) {
        session->httpBuffer += chunk;
        const wsproto::HttpRequest request = wsproto::parseHttpRequest(session->httpBuffer);
        if (!request.error.isEmpty()) {
            rejectSession(session, 400, request.error);
            return;
        }
        if (!request.complete)
            return;   // 请求头还没收全，等下一次 readyRead

        int status = 400;
        const QString problem = wsproto::validateHandshake(request, &status);
        if (!problem.isEmpty()) {
            rejectSession(session, status, problem);
            return;
        }
        QString protocolError;
        const QString protocol = wsproto::selectProtocol(request, m_protocols, &protocolError);
        if (!protocolError.isEmpty()) {
            rejectSession(session, 400, protocolError);
            return;
        }

        const QByteArray key = request.headers.value(QStringLiteral("sec-websocket-key")).toLatin1();
        session->socket->write(wsproto::buildAcceptResponse(key, protocol));
        session->socket->flush();

        session->open     = true;
        session->protocol = protocol;
        session->target   = request.target;
        emit clientConnected(session->id, session->peer, protocol);

        // 请求头后面可能紧跟客户端的第一条消息，别丢掉
        const QByteArray rest = session->httpBuffer.mid(request.length);
        session->httpBuffer.clear();
        if (!rest.isEmpty()) {
            session->reader.feed(rest);
            pumpFrames(session);
        }
        return;
    }

    if (session->closing)
        return;   // 已发出关闭帧：对端还没断时的数据一律忽略
    session->reader.feed(chunk);
    pumpFrames(session);
}

void WsServer::rejectSession(Session *session, int status, const QString &reason)
{
    if (session == nullptr || session->rejected)
        return;
    session->rejected    = true;
    session->closeReason = reason;
    emit protocolNotice(session->id,
                        QStringLiteral("已拒绝握手（HTTP %1）：%2").arg(status).arg(reason));

    if (session->socket != nullptr) {
        session->socket->write(wsproto::buildRejectResponse(status, reason));
        session->socket->flush();
        // 只断开、不 abort()：abort 会丢弃未送达的字节，客户端可能只看到"连接被重置"
        // 而拿不到 4xx 的原因。收尾交给 disconnected -> onDisconnected（open == false
        // 走静默销毁），对端不配合也不会留下会话。
        session->socket->disconnectFromHost();
    }

    // 对端读完 4xx 后通常自己关；不关的（脚本、连接池）到点强制收掉
    const QString id = session->id;
    QTimer::singleShot(kRejectGraceMs, this, [this, id] {
        Session *pending = sessionFor(id);
        if (pending != nullptr)
            destroySession(pending, pending->closeReason, false);
    });
}

// ---------------------------------------------------------------- 帧循环

void WsServer::pumpFrames(Session *session)
{
    if (session == nullptr)
        return;

    wsproto::Frame frame;
    while (session->reader.next(&frame)) {
        switch (frame.opcode) {
        case wsproto::OpText:
        case wsproto::OpBinary:
            if (session->fragOpcode != 0) {
                emit protocolNotice(session->id, QStringLiteral("协议错误：上一条分片消息还没结束就来了新消息"));
                sendClose(session, 1002, QStringLiteral("协议错误"));
                return;
            }
            if (frame.fin) {
                if (frame.opcode == wsproto::OpText)
                    emit textReceived(session->id, QString::fromUtf8(frame.payload));
                else
                    emit binaryReceived(session->id, frame.payload.size());
            } else {
                session->fragOpcode = frame.opcode;
                session->fragPayload = frame.payload;
            }
            break;

        case wsproto::OpContinuation:
            if (session->fragOpcode == 0) {
                emit protocolNotice(session->id, QStringLiteral("协议错误：收到没有起始帧的续帧"));
                sendClose(session, 1002, QStringLiteral("协议错误"));
                return;
            }
            if (static_cast<quint64>(session->fragPayload.size() + frame.payload.size())
                > wsproto::kMaxMessageBytes) {
                sendClose(session, 1009, QStringLiteral("消息超过上限"));
                return;
            }
            session->fragPayload += frame.payload;
            if (frame.fin) {
                if (session->fragOpcode == wsproto::OpText)
                    emit textReceived(session->id, QString::fromUtf8(session->fragPayload));
                else
                    emit binaryReceived(session->id, session->fragPayload.size());
                session->fragOpcode = 0;
                session->fragPayload.clear();
            }
            break;

        case wsproto::OpPing:
            // 回 pong 时必须带同样的载荷（RFC 6455 §5.5.3），否则有些客户端不认
            sendFrame(session, wsproto::OpPong, frame.payload);
            emit protocolNotice(session->id, QStringLiteral("收到 ping（%1 字节），已回 pong")
                                                 .arg(frame.payload.size()));
            break;

        case wsproto::OpPong:
            // 本服务端不主动 ping；客户端自发回 pong 时只记一行，便于确认链路
            emit protocolNotice(session->id, QStringLiteral("收到 pong（%1 字节）")
                                                 .arg(frame.payload.size()));
            break;

        case wsproto::OpClose: {
            int code = 0;
            QString reason;
            if (frame.payload.size() >= 2) {
                code = (static_cast<quint8>(frame.payload.at(0)) << 8)
                       | static_cast<quint8>(frame.payload.at(1));
                reason = QString::fromUtf8(frame.payload.mid(2));
            }
            QString text;
            if (code != 0)
                text = QStringLiteral("客户端关闭：%1（%2）").arg(code).arg(wsproto::closeCodeText(code));
            else
                text = QStringLiteral("客户端关闭（未带状态码）");
            if (!reason.isEmpty())
                text += QStringLiteral("，原因：%1").arg(reason);

            emit protocolNotice(session->id, text);
            if (!session->closing) {
                sendFrame(session, wsproto::OpClose, frame.payload.left(125));
                session->closing     = true;
                session->closeReason = text;
            }
            if (session->socket != nullptr)
                session->socket->disconnectFromHost();
            break;
        }

        default:
            break;
        }
    }

    if (session->reader.failed()) {
        // 协议错误要回 1002 再断，否则对端只能看到"连接没了"
        emit protocolNotice(session->id, session->reader.error());
        sendClose(session, 1002, QStringLiteral("协议错误"));
    }
}

// ---------------------------------------------------------------- 收尾

void WsServer::sendClose(Session *session, int code, const QString &reason)
{
    if (session == nullptr || session->closing || session->socket == nullptr)
        return;

    session->closing     = true;
    session->closeReason = reason.isEmpty() ? QStringLiteral("服务端关闭连接") : reason;
    sendFrame(session, wsproto::OpClose, wsproto::closePayload(code, reason));
    session->socket->flush();
    session->socket->disconnectFromHost();

    // 对端不配合（不回 FIN）时兜底收掉，否则会话会一直挂在表里
    const QString id = session->id;
    QTimer::singleShot(kCloseTimeoutMs, this, [this, id] {
        Session *pending = sessionFor(id);
        if (pending != nullptr)
            destroySession(pending, pending->closeReason, true);
    });
}

void WsServer::onDisconnected(const QString &id)
{
    Session *session = sessionFor(id);
    if (session == nullptr)
        return;

    if (!session->open) {
        destroySession(session, session->closeReason, false);
        return;
    }
    const QString reason = session->closeReason.isEmpty()
                               ? QStringLiteral("对端断开（没有收到关闭帧）")
                               : session->closeReason;
    destroySession(session, reason, true);
}

void WsServer::destroySession(Session *session, const QString &reason, bool announce)
{
    if (session == nullptr)
        return;

    const QString id     = session->id;
    const bool    wasOpen = session->open;
    m_sessions.remove(id);
    m_order.removeAll(id);

    QTcpSocket *socket = session->socket;
    session->socket = nullptr;
    if (socket != nullptr) {
        // 先断信号再 abort：否则 abort() 触发的 disconnected 会绕回 onDisconnected，
        // 对着已经 delete 的 Session 再走一遍
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    delete session;

    if (announce && wasOpen)
        emit clientDisconnected(id, reason);
}

bool WsServer::sendFrame(Session *session, int opcode, const QByteArray &payload)
{
    if (session == nullptr || session->socket == nullptr)
        return false;
    // 服务端 -> 客户端：不掩码（mask = false）
    const QByteArray frame = wsproto::encodeFrame(opcode, payload, false);
    return session->socket->write(frame) == frame.size();
}

WsServer::Session *WsServer::sessionFor(const QString &id) const
{
    return m_sessions.value(id);
}

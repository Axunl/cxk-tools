#include "WsClient.h"

#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>

namespace {
/// 握手等待 101 响应的上限。局域网 / 公网正常都在 1 秒内，8 秒足够覆盖
/// 慢链路；再久下去用户只会以为工具卡死。
constexpr int kHandshakeTimeoutMs = 8000;
/// 发完关闭帧后等对端回 close 的上限，超时就 abort（RFC 允许直接断）。
constexpr int kCloseTimeoutMs = 2000;
/// 心跳间隔与等待 pong 的时间。
constexpr int kHeartbeatIntervalMs = 25000;
constexpr int kPongTimeoutMs = 10000;
} // namespace

WsClient::WsClient(QObject *parent)
    : QObject(parent)
{
    m_handshakeTimer = new QTimer(this);
    m_handshakeTimer->setSingleShot(true);
    connect(m_handshakeTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Connecting)
            fail(QStringLiteral("握手超时：%1 秒内没有收到 101 响应").arg(kHandshakeTimeoutMs / 1000));
    });

    m_closeTimer = new QTimer(this);
    m_closeTimer->setSingleShot(true);
    connect(m_closeTimer, &QTimer::timeout, this, [this] {
        if (m_state != State::Closing)
            return;
        const QString reason = m_closeReason.isEmpty()
                                   ? QStringLiteral("关闭超时，已强制断开")
                                   : m_closeReason;
        teardownSocket();
        stopTimers();
        m_state = State::Idle;
        emit stateChanged(State::Idle, reason);
    });

    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &WsClient::onHeartbeatTick);

    m_pongTimer = new QTimer(this);
    m_pongTimer->setSingleShot(true);
    connect(m_pongTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Open && m_pingPending)
            fail(QStringLiteral("心跳超时：%1 秒内没有收到 pong，连接已判定断开")
                     .arg(kPongTimeoutMs / 1000));
    });
}

WsClient::~WsClient()
{
    // 面板可能带着连接直接被销毁（切换工具页、关独立窗口）：静默 abort，
    // 不发信号、不留后台 socket。宿主不缓存面板，所以这是常见路径。
    teardownSocket();
}

void WsClient::open(const wsproto::Url &url, const QStringList &protocols,
                    const QStringList &extraHeaders, bool ignoreCertificate)
{
    if (m_state != State::Idle)
        return;   // 已经在连接/已连接，忽略重复点击

    resetSession();
    m_url = url;
    m_protocols = protocols;
    m_extraHeaders = extraHeaders;
    m_ignoreCertificate = ignoreCertificate;
    m_key = wsproto::makeKey();
    m_state = State::Connecting;
    emit stateChanged(State::Connecting,
                      QStringLiteral("正在连接 %1://%2:%3%4")
                          .arg(url.secure ? QStringLiteral("wss") : QStringLiteral("ws"),
                               url.host)
                          .arg(url.port)
                          .arg(url.target));

    if (url.secure) {
        if (!QSslSocket::supportsSsl()) {
            fail(QStringLiteral("当前 Qt 没有可用的 TLS 后端，无法连接 wss://（构建时用的 "
                                "Qt 未带 OpenSSL / Schannel 插件）"));
            return;
        }
        auto *ssl = new QSslSocket(this);
        m_socket = ssl;
        // 握手完成信号是 encrypted 而不是 connected（后者只代表 TCP 通了）
        connect(ssl, &QSslSocket::encrypted, this, &WsClient::onConnected);
        connect(ssl, &QSslSocket::sslErrors, this, &WsClient::onSslErrors);
    } else {
        m_socket = new QTcpSocket(this);
        connect(m_socket, &QTcpSocket::connected, this, &WsClient::onConnected);
    }

    connect(m_socket, &QTcpSocket::readyRead, this, &WsClient::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &WsClient::onSocketError);
    connect(m_socket, &QTcpSocket::disconnected, this, &WsClient::onDisconnected);

    if (url.secure)
        static_cast<QSslSocket *>(m_socket)->connectToHostEncrypted(url.host, url.port, url.host);
    else
        m_socket->connectToHost(url.host, url.port);
}

void WsClient::close(int code, const QString &reason)
{
    if (m_state == State::Idle || m_socket == nullptr)
        return;

    if (m_state != State::Open) {
        // 还在连接中：用户点的是「取消」，不必发关闭帧（握手还没完成，发了也没意义）
        m_closeReason = QStringLiteral("已取消连接");
        teardownSocket();
        stopTimers();
        m_state = State::Idle;
        emit stateChanged(State::Idle, m_closeReason);
        return;
    }

    sendFrame(wsproto::OpClose, wsproto::closePayload(code, reason));
    m_socket->flush();
    m_closeReason = reason.isEmpty() ? QStringLiteral("已主动断开") : reason;
    m_state = State::Closing;
    m_heartbeatTimer->stop();
    m_pongTimer->stop();
    m_closeTimer->start(kCloseTimeoutMs);
    emit stateChanged(State::Closing, m_closeReason);
    // 正常关机流程：先发关闭帧，再等对端也关；对端不配合时由 closeTimer 兜底
    m_socket->disconnectFromHost();
}

bool WsClient::sendText(const QString &text)
{
    if (m_state != State::Open)
        return false;
    return sendFrame(wsproto::OpText, text.toUtf8());
}

void WsClient::setHeartbeatEnabled(bool enabled)
{
    m_heartbeatEnabled = enabled;
    if (!enabled) {
        m_heartbeatTimer->stop();
        m_pongTimer->stop();
        m_pingPending = false;
        return;
    }
    if (m_state == State::Open) {
        m_heartbeatTimer->start(kHeartbeatIntervalMs);
        emit protocolNotice(QStringLiteral("已开启心跳：每 %1 秒发一次 ping，%2 秒无 pong 判为断线")
                                .arg(kHeartbeatIntervalMs / 1000)
                                .arg(kPongTimeoutMs / 1000));
    }
}

void WsClient::onConnected()
{
    if (m_socket == nullptr || m_state != State::Connecting)
        return;

    const QByteArray request =
        wsproto::buildHandshake(m_url, m_key, m_protocols, m_extraHeaders).toUtf8();
    m_socket->write(request);
    m_socket->flush();
    m_handshakeTimer->start(kHandshakeTimeoutMs);
}

void WsClient::onReadyRead()
{
    if (m_socket == nullptr)
        return;

    const QByteArray chunk = m_socket->readAll();
    if (chunk.isEmpty())
        return;

    if (m_state == State::Connecting) {
        // 握手阶段：先把整个响应头收齐，再判断 101 与 Sec-WebSocket-Accept
        m_httpBuffer += chunk;
        const wsproto::HttpHead head = wsproto::parseHttpHead(m_httpBuffer);
        if (!head.error.isEmpty()) {
            fail(QStringLiteral("握手失败：%1").arg(head.error));
            return;
        }
        if (!head.complete)
            return;   // 头还没收完，等下一次 readyRead

        m_handshakeTimer->stop();

        if (head.status != 101) {
            fail(QStringLiteral("服务端拒绝了握手：HTTP %1（期望 101 Switching Protocols）")
                     .arg(head.status));
            return;
        }
        if (!wsproto::checkAccept(m_key, head.headers.value(QStringLiteral("sec-websocket-accept")))) {
            fail(QStringLiteral("握手校验失败：Sec-WebSocket-Accept 不匹配，"
                                "对方可能不是 WebSocket 服务，或被代理改写"));
            return;
        }

        m_protocol = head.headers.value(QStringLiteral("sec-websocket-protocol"));
        if (!m_protocol.isEmpty() && !m_protocols.contains(m_protocol, Qt::CaseInsensitive)) {
            fail(QStringLiteral("服务端选了没有申请的子协议：%1").arg(m_protocol));
            return;
        }

        m_state = State::Open;
        emit stateChanged(State::Open,
                          m_protocol.isEmpty()
                              ? QStringLiteral("已连接")
                              : QStringLiteral("已连接（子协议 %1）").arg(m_protocol));
        if (m_heartbeatEnabled)
            m_heartbeatTimer->start(kHeartbeatIntervalMs);

        // 响应头后面可能紧跟着服务端的第一条消息，别把它丢掉
        const QByteArray rest = m_httpBuffer.mid(head.length);
        m_httpBuffer.clear();
        if (!rest.isEmpty()) {
            m_reader.feed(rest);
            pumpFrames();
        }
        return;
    }

    m_reader.feed(chunk);
    pumpFrames();
}

void WsClient::pumpFrames()
{
    wsproto::Frame frame;
    while (m_reader.next(&frame)) {
        switch (frame.opcode) {
        case wsproto::OpText:
        case wsproto::OpBinary:
            if (m_fragOpcode != 0) {
                fail(QStringLiteral("协议错误：上一条分片消息还没结束就来了新消息"));
                return;
            }
            if (frame.fin) {
                deliverMessage(frame.opcode, frame.payload);
            } else {
                m_fragOpcode = frame.opcode;
                m_fragPayload = frame.payload;
            }
            break;

        case wsproto::OpContinuation:
            if (m_fragOpcode == 0) {
                fail(QStringLiteral("协议错误：收到没有起始帧的续帧"));
                return;
            }
            if (static_cast<quint64>(m_fragPayload.size() + frame.payload.size())
                > wsproto::kMaxMessageBytes) {
                fail(QStringLiteral("消息超过 %1 MB 上限，已断开")
                         .arg(wsproto::kMaxMessageBytes / 1024 / 1024));
                return;
            }
            m_fragPayload += frame.payload;
            if (frame.fin) {
                deliverMessage(m_fragOpcode, m_fragPayload);
                m_fragOpcode = 0;
                m_fragPayload.clear();
            }
            break;

        case wsproto::OpPing:
            sendFrame(wsproto::OpPong, frame.payload);
            emit protocolNotice(QStringLiteral("收到 ping（%1 字节），已回 pong")
                                    .arg(frame.payload.size()));
            break;

        case wsproto::OpPong:
            m_pingPending = false;
            m_pongTimer->stop();
            emit protocolNotice(QStringLiteral("心跳正常（收到 pong）"));
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
            if (code != 0) {
                text = QStringLiteral("服务端关闭连接：%1（%2）")
                           .arg(code)
                           .arg(wsproto::closeCodeText(code));
            } else {
                text = QStringLiteral("服务端关闭连接（未带状态码）");
            }
            if (!reason.isEmpty())
                text += QStringLiteral("，原因：%1").arg(reason);

            if (m_state == State::Open)
                sendFrame(wsproto::OpClose, frame.payload.left(125));
            m_closeReason = text;
            m_state = State::Closing;
            m_heartbeatTimer->stop();
            m_pongTimer->stop();
            m_closeTimer->start(kCloseTimeoutMs);
            emit protocolNotice(text);
            emit stateChanged(State::Closing, text);
            if (m_socket)
                m_socket->disconnectFromHost();
            break;
        }

        default:
            break;
        }
    }

    if (m_reader.failed())
        fail(m_reader.error());
}

void WsClient::deliverMessage(int opcode, const QByteArray &payload)
{
    if (opcode == wsproto::OpText) {
        emit textReceived(QString::fromUtf8(payload));
    } else {
        // 二进制帧只报大小：本工具定位是「简易连接 + 收发文本」，
        // 展示二进制需要在面板里做十六进制视图，超出本次范围（见 README 已知边界）
        emit binaryReceived(payload.size());
    }
}

void WsClient::onSocketError()
{
    if (m_socket == nullptr)
        return;

    // 对端直接关 TCP（没有关闭帧）时 Qt 会先报 RemoteHostClosedError 再发 disconnected：
    // 这里不拦的话日志里会先冒一条"连接出错"，而实际上这是很常见的正常收尾。
    if (m_socket->error() == QAbstractSocket::RemoteHostClosedError)
        return;
    // 主动关闭期间的错误（连接被重置等）不再当成新的失败：关闭流程由 closeTimer 收尾
    if (m_state == State::Closing)
        return;

    fail(QStringLiteral("连接出错：%1").arg(m_socket->errorString()));
}

void WsClient::onDisconnected()
{
    if (m_state == State::Idle)
        return;

    const QString reason = m_closeReason.isEmpty()
                               ? QStringLiteral("连接已断开（对端关闭或网络中断）")
                               : m_closeReason;
    teardownSocket();
    stopTimers();
    m_state = State::Idle;
    emit stateChanged(State::Idle, reason);
}

void WsClient::onSslErrors(const QList<QSslError> &errors)
{
    auto *ssl = qobject_cast<QSslSocket *>(m_socket);
    if (ssl == nullptr)
        return;

    if (m_ignoreCertificate) {
        ssl->ignoreSslErrors();
        emit protocolNotice(QStringLiteral("已忽略证书校验错误（勾选了「跳过证书校验」）"));
        return;
    }

    QStringList details;
    for (const QSslError &error : errors)
        details << error.errorString();
    fail(QStringLiteral("TLS 证书校验失败：%1。确需连接可勾选「跳过证书校验」")
             .arg(details.join(QStringLiteral("；"))));
}

void WsClient::onHeartbeatTick()
{
    if (m_state != State::Open)
        return;
    if (m_pingPending) {
        // 上一轮 ping 还没等到 pong：多半是半开连接，直接判定断线
        fail(QStringLiteral("心跳超时：%1 秒内没有收到 pong，连接已判定断开")
                 .arg(kPongTimeoutMs / 1000));
        return;
    }
    if (sendFrame(wsproto::OpPing, QByteArrayLiteral("hb"))) {
        m_pingPending = true;
        m_pongTimer->start(kPongTimeoutMs);
    }
}

bool WsClient::sendFrame(int opcode, const QByteArray &payload)
{
    if (m_socket == nullptr)
        return false;
    const QByteArray frame = wsproto::encodeFrame(opcode, payload);
    return m_socket->write(frame) == frame.size();
}

void WsClient::stopTimers()
{
    m_handshakeTimer->stop();
    m_closeTimer->stop();
    m_heartbeatTimer->stop();
    m_pongTimer->stop();
    m_pingPending = false;
}

void WsClient::teardownSocket()
{
    QTcpSocket *socket = m_socket;
    if (socket == nullptr)
        return;
    m_socket = nullptr;
    // 先切断信号再 abort：否则 abort() 里的 disconnected 会绕回 onDisconnected，
    // 在 fail() 已经报过错之后再报一次"连接已断开"，日志里出现两条互相矛盾的记录。
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

void WsClient::fail(const QString &reason)
{
    if (m_state == State::Idle)
        return;

    teardownSocket();
    stopTimers();
    m_state = State::Idle;
    m_fragOpcode = 0;
    m_fragPayload.clear();
    m_httpBuffer.clear();
    m_reader.reset();
    m_pingPending = false;

    emit failure(reason);
    emit stateChanged(State::Idle, reason);
}

void WsClient::resetSession()
{
    m_reader.reset();
    m_httpBuffer.clear();
    m_key.clear();
    m_protocol.clear();
    m_closeReason.clear();
    m_fragOpcode = 0;
    m_fragPayload.clear();
    m_pingPending = false;
    m_state = State::Idle;
}

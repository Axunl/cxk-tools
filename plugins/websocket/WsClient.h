#pragma once

// ---------------------------------------------------------------------------
// WebSocket 调试插件 —— 连接层。
//
// 职责：建立 TCP/TLS 连接、发握手、维护帧收发、心跳与关闭。
// 协议细节全在 WsProtocol.h（纯逻辑、可单独测试），这里只负责把 socket 的
// 字节流喂给解析器、把语义化的结果用信号抛给面板。
//
// 平台：只用 QTcpSocket / QSslSocket，没有任何平台分支 —— 三端的差异都在
// Qt Network 内部（Windows 走 Schannel，Linux/macOS 走 OpenSSL 或系统后端）。
// 拿不到 TLS 后端时（Qt 未带 openssl）会给明确文案，不静默失败。
//
// 生命周期：一个 WsClient 对应一次连接尝试；面板析构时随父对象一起销毁，
// 析构里 abort()，所以"面板关了但 socket 还在后台连"的情况不会发生。
// ---------------------------------------------------------------------------

#include "WsProtocol.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <QSslError>

class QTcpSocket;
class QTimer;

class WsClient : public QObject
{
    Q_OBJECT

public:
    /// 连接状态。面板按它决定按钮文案与输入框可用性。
    enum class State {
        Idle,        ///< 没有连接（也包括刚断开）
        Connecting,  ///< TCP/TLS 连接中，或已发出握手等待 101
        Open,        ///< 握手完成，可收发
        Closing,     ///< 已发/已收关闭帧，等对端断开
    };

    explicit WsClient(QObject *parent = nullptr);
    ~WsClient() override;

    /// 发起连接。url 必须已通过 wsproto::parseUrl() 校验。
    void open(const wsproto::Url &url, const QStringList &protocols,
              const QStringList &extraHeaders, bool ignoreCertificate);

    /// 主动关闭：发关闭帧，2 秒内对端不断开就 abort。
    void close(int code = 1000, const QString &reason = QString());

    /// 发送一条文本消息。只有 Open 状态会真正发出，返回是否发出。
    bool sendText(const QString &text);

    /// 开关心跳：每 25 秒发一次 ping；上一次 ping 还没收到 pong 就判定断线。
    void setHeartbeatEnabled(bool enabled);

    State   state() const { return m_state; }
    QString negotiatedProtocol() const { return m_protocol; }

signals:
    /// 状态变化。detail 是给人看的中文说明（已连接 / 服务端关闭…）。
    void stateChanged(WsClient::State state, const QString &detail);

    /// 收到一条完整的文本消息（分片已拼好）。
    void textReceived(const QString &text);

    /// 收到二进制消息。本工具只展示大小，不解析内容。
    void binaryReceived(qint64 bytes);

    /// 协议层事件：收到 ping / pong、关闭码等，用于日志。
    void protocolNotice(const QString &text);

    /// 连接失败或协议错误。发出后状态一定是 Idle。
    void failure(const QString &reason);

private slots:
    void onConnected();          ///< 明文 TCP 连上，或 TLS 握手完成
    void onReadyRead();
    void onSocketError();
    void onDisconnected();
    void onSslErrors(const QList<QSslError> &errors);
    void onHeartbeatTick();

private:
    bool sendFrame(int opcode, const QByteArray &payload);
    void pumpFrames();
    void deliverMessage(int opcode, const QByteArray &payload);
    void startHeartbeatTimer();
    void stopTimers();
    void teardownSocket();
    void fail(const QString &reason);
    void resetSession();

    State    m_state = State::Idle;
    QTcpSocket *m_socket = nullptr;   ///< 实际是 QTcpSocket 或 QSslSocket（多态使用）

    QTimer *m_handshakeTimer = nullptr;   ///< 8 秒没拿到 101 就判失败
    QTimer *m_closeTimer     = nullptr;   ///< 发关闭帧后 2 秒强制 abort
    QTimer *m_heartbeatTimer = nullptr;   ///< 25 秒一次 ping
    QTimer *m_pongTimer      = nullptr;   ///< ping 发出后 10 秒等 pong

    wsproto::Url    m_url;
    QStringList     m_extraHeaders;    ///< 已规整的自定义请求头
    QByteArray      m_key;            ///< 本次握手的 Sec-WebSocket-Key
    QByteArray      m_httpBuffer;     ///< 握手响应头（还没收全时累积）
    wsproto::FrameReader m_reader;    ///< 握手完成后解析帧

    QStringList m_protocols;          ///< 申请的子协议
    QString     m_protocol;           ///< 服务端选定的子协议

    bool    m_ignoreCertificate = false;
    bool    m_heartbeatEnabled  = false;
    bool    m_pingPending       = false;   ///< 已发 ping 还没收到 pong
    QString m_closeReason;                 ///< 关闭原因，断线时显示

    int     m_fragOpcode = 0;         ///< 分片消息的起始 opcode，0 表示没有在拼
    QByteArray m_fragPayload;         ///< 分片累积缓冲
};

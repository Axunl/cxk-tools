#pragma once

// ---------------------------------------------------------------------------
// WebSocket 调试插件 —— 服务端连接层。
//
// 职责：监听端口、接受并校验客户端的握手、按会话收发帧、群发与主动断开。
// 协议细节全在 WsProtocol.h（纯逻辑、可单独测试），这里只负责把 QTcpSocket 的
// 字节流喂给解析器、把语义化结果用信号抛给面板。
//
// 为什么要有服务端：
//   调试客户端时经常需要一个"对面"，而公网 echo 服务不能自定义响应、也不能
//   主动推消息。本机起一个能看到的服务端，既能验证自己写的客户端，也能让
//   浏览器 / 手机 / 局域网的设备连进来，把收发过程直接打在日志里。
//
// 边界：
//   * 只做 ws://（明文）。TLS 监听要证书与私钥输入，超出"简易"范围；
//   * 一个 WsServer 对应一次监听，停掉后会话全部断开；
//   * 不做自动心跳（由客户端发 ping，本服务端必定回 pong）。
//
// 生命周期：面板析构时随父对象销毁，析构里 stop() —— 面板不缓存，
// 离开工具页即停止监听，不会留下后台端口。
// ---------------------------------------------------------------------------

#include "WsProtocol.h"

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QTcpServer;

class WsServer : public QObject
{
    Q_OBJECT

public:
    /// 已通过握手、正在通信的客户端。id 形如 `c1`，只在本次监听内唯一。
    struct Client
    {
        QString id;
        QString peer;       ///< 对端 "ip:port"，日志里用来区分是谁
        QString protocol;   ///< 协商出的子协议，没协商就是空
        QString target;     ///< 客户端请求的路径，例如 /ws
    };

    explicit WsServer(QObject *parent = nullptr);
    ~WsServer() override;

    ///
    /// 开始监听。port = 0 表示由系统分配（面板会把实际端口读回去显示）。
    /// protocols 为空表示"回显客户端提出的第一个子协议"，否则只接受交集。
    /// 失败时发出 failure()，返回 false。
    ///
    bool listen(const QHostAddress &address, quint16 port, const QStringList &protocols);

    /// 停止监听并断开所有客户端（发 1001 关闭帧）。已停止时是空操作。
    void stop(const QString &reason = QString());

    bool    isListening() const;
    quint16 port() const;          ///< 实际监听端口（listen 成功后才有意义）
    int     clientCount() const;
    QList<Client> clients() const;

    /// 发给指定客户端。客户端不存在或还没握手完成时返回 false。
    bool sendText(const QString &clientId, const QString &text);

    /// 群发，返回成功发出的客户端数量。没有客户端时返回 0。
    int broadcastText(const QString &text);

    /// 服务端主动断开某个客户端：先发关闭帧，再断 TCP。
    bool disconnectClient(const QString &clientId, int code = 1000,
                          const QString &reason = QString());

signals:
    void listening(quint16 port);
    void stopped(const QString &reason);
    void failure(const QString &reason);

    /// 握手成功、可以收发。id 是后续所有信号的第一个参数。
    void clientConnected(const QString &id, const QString &peer, const QString &protocol);
    void clientDisconnected(const QString &id, const QString &reason);
    void textReceived(const QString &id, const QString &text);
    void binaryReceived(const QString &id, qint64 bytes);

    /// 单个客户端的协议事件（收到 ping / 握手被拒等），用于日志。
    void protocolNotice(const QString &id, const QString &text);

private:
    struct Session;

    void onNewConnection();
    void onReadyRead(const QString &id);
    void onDisconnected(const QString &id);
    void pumpFrames(Session *session);
    void rejectSession(Session *session, int status, const QString &reason);
    void sendClose(Session *session, int code, const QString &reason);
    void destroySession(Session *session, const QString &reason, bool announce);
    void stopInternal(const QString &reason, bool announce);
    bool sendFrame(Session *session, int opcode, const QByteArray &payload);
    Session *sessionFor(const QString &id) const;

    QTcpServer *m_server = nullptr;
    QStringList  m_protocols;
    QHash<QString, Session *> m_sessions;
    QStringList m_order;   ///< 接入顺序（QHash 无序，面板列表要稳定顺序）
    int m_nextId = 0;
};

#include "SingleInstance.h"

#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

namespace {
constexpr auto kActivateMessage = "activate";
// 探测超时。取 300ms：正常情况本地 socket 连接是亚毫秒级，
// 这个值只为覆盖极端卡顿，同时保证「另一个实例刚好在退出」时不会卡住启动。
constexpr int kProbeTimeoutMs = 300;
} // namespace

SingleInstance::SingleInstance(const QString &key, QObject *parent)
    : QObject(parent)
    , m_key(key)
{
}

bool SingleInstance::tryLock()
{
    // 1) 先探测：能连上说明已有实例在跑，把 activate 消息丢过去让它唤出窗口
    QLocalSocket probe;
    probe.connectToServer(m_key);
    if (probe.waitForConnected(kProbeTimeoutMs)) {
        probe.write(kActivateMessage);
        probe.flush();
        probe.waitForBytesWritten(kProbeTimeoutMs);
        probe.disconnectFromServer();
        return false;
    }

    // 2) 没有实例：清掉上次崩溃残留的 socket 文件再 listen。
    //    Unix 上进程被 kill -9 会留下 socket 文件，不清理会 listen 失败。
    QLocalServer::removeServer(m_key);

    m_server = new QLocalServer(this);
    if (!m_server->listen(m_key)) {
        // 3) listen 失败（权限/残留文件占用）时退化为「允许多开」，
        //    而不是让用户完全打不开应用 —— 能多开总比不能用好。
        qWarning() << "SingleInstance: listen failed:" << m_server->errorString()
                   << "，本次启动不启用单实例限制";
        delete m_server;
        m_server = nullptr;
        return true;
    }

    connect(m_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
    return true;
}

void SingleInstance::setActivateHandler(std::function<void()> handler)
{
    m_handler = std::move(handler);
}

void SingleInstance::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
        socket->waitForReadyRead(50);
        const QByteArray payload = socket->readAll();
        socket->disconnectFromServer();
        qInfo() << "SingleInstance: 收到 activate 请求" << payload;

        if (m_handler) {
            // 延后到事件循环下一轮：让 socket 先干净地关掉，
            // 也避免在信号回调里直接 raise/activate 窗口。
            const auto handler = m_handler;
            QTimer::singleShot(0, this, [handler] {
                if (handler)
                    handler();
            });
        }
    }
}

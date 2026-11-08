#include "WebSocketTool.h"

#include "WebSocketPanel.h"

// 面板类带 Q_OBJECT，定义在 WebSocketPanel.h 里（AUTOMOC 直接处理），
// 所以这里不需要再 #include "xxx.moc"。
// 本文件只保留插件入口：工具元信息 + 建面板。

xtools::ToolInfo WebSocketTool::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("websocket");
    info.name = QStringLiteral("WebSocket 调试");
    info.keywords = QStringList{QStringLiteral("websocket"), QStringLiteral("ws"),
                                QStringLiteral("wss"), QStringLiteral("socket"),
                                QStringLiteral("连接"), QStringLiteral("长连接"),
                                QStringLiteral("服务端"), QStringLiteral("server"),
                                QStringLiteral("监听"), QStringLiteral("调试"),
                                QStringLiteral("推送")};
    info.description = QStringLiteral("连到 ws:// / wss:// 服务收发文本，或在本机起一个 ws:// 服务端让其他客户端连进来");
    return info;
}

QWidget *WebSocketTool::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存面板，每次进入工具都会调用这里。
    // 面板内部同时持有客户端页与服务端页，切换不会断开正在跑的连接/监听。
    return new WebSocketPanel(parent);
}

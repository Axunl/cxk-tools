#pragma once

// ---------------------------------------------------------------------------
// WebSocket 调试插件：连到 ws:// / wss:// 服务收发文本，或在本机起一个 ws:// 服务端。
//
// 实现分层（与本仓库 procmon / colorpicker 的组织方式一致）：
//   WsProtocol.h      纯协议逻辑（地址解析、双方握手、帧编解码），无 socket 无 UI，可单独测试；
//   WsClient.*        客户端连接层（QTcpSocket / QSslSocket），只发语义化信号；
//   WsServer.*        服务端连接层（QTcpServer），按会话管理握手与收发；
//   WebSocketPanel.*  面板（客户端页 / 服务端页 / 容器）。
//
// 不依赖 Qt WebSockets 模块：本项目锁定的 Qt 6.8.3 安装里没有它，
// 两端都用 Qt Network 自己实现（原因见 WsProtocol.h 文件头）。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class WebSocketTool : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

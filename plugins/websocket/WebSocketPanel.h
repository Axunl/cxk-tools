#pragma once

// ---------------------------------------------------------------------------
// WebSocket 调试插件 —— 面板。
//
// 分成三个类：
//   WsClientPage  连到别人的 ws:// / wss:// 服务，收发文本；
//   WsServerPage  在本机起一个 ws:// 服务端，等别人连进来；
//   WebSocketPanel  容器：顶部分段开关 + QStackedWidget。
//
// 为什么两页同时存活（而不是切页时销毁重建）：
//   调试时经常要「自己当服务端，再用浏览器/别的客户端连」，或者「客户端连着，
//   临时起个服务端给同事连」；切一下页面就把连接断掉会非常碍事。
//   代价是两页的 socket / 监听一直占着，关面板（宿主销毁面板）时一起收掉。
//
// 面板不设 parent、每次 createPanel() 都新建，与宿主「面板不缓存」的约定一致；
// 所有状态都在实例里，不用静态变量（内嵌与独立窗口可能同时存在两份）。
// ---------------------------------------------------------------------------

#include "WsClient.h"
#include "ThemePalette.h"   // 状态行/日志的颜色令牌

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class WsServer;

/// 客户端页：地址栏 + 握手选项 + 消息日志 + 发送框。
class WsClientPage : public QWidget
{
    Q_OBJECT

public:
    explicit WsClientPage(QWidget *parent = nullptr);

private:
    QWidget *buildConnectRow(QVBoxLayout *layout);
    QWidget *buildOptionsRow(QVBoxLayout *layout);
    void wireSignals();

    void toggleConnection();
    void startConnection();
    void sendCurrent();

    void applyState(WsClient::State state, const QString &detail);
    void setConfigEnabled(bool enabled);
    void setStatus(const QString &text, xtools::theme::Color token);
    void appendLog(const QString &text, xtools::theme::Color token);

    WsClient      *m_client = nullptr;
    QLineEdit     *m_url = nullptr;
    QPushButton   *m_connectButton = nullptr;
    QLineEdit     *m_protocol = nullptr;
    QCheckBox     *m_ignoreCert = nullptr;
    QCheckBox     *m_heartbeat = nullptr;
    QPlainTextEdit *m_headers = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton   *m_clearLog = nullptr;
    QPlainTextEdit *m_send = nullptr;
    QPushButton   *m_sendButton = nullptr;
    QLabel        *m_status = nullptr;

    /// 本次断开是否已经由 failure() 记录过，避免 Idle 状态重复刷同一条原因。
    bool m_failureLogged = false;
};

/// 服务端页：监听设置 + 客户端列表 + 消息日志 + 发送框。
class WsServerPage : public QWidget
{
    Q_OBJECT

public:
    explicit WsServerPage(QWidget *parent = nullptr);

private:
    QWidget *buildListenRows(QVBoxLayout *layout);
    void wireSignals();

    void toggleListening();
    void onListening(quint16 port);
    void onStopped(const QString &reason);
    void onFailure(const QString &reason);
    void refreshClients();
    void sendCurrent();
    void copyAddress();

    QStringList selectedIds() const;
    void setConfigEnabled(bool enabled);
    void setStatus(const QString &text, xtools::theme::Color token);
    void updateStatusSummary();
    void appendLog(const QString &text, xtools::theme::Color token);
    QString connectUrl(quint16 port) const;

    WsServer      *m_server = nullptr;
    QLineEdit     *m_port = nullptr;
    QPushButton   *m_listenButton = nullptr;
    QCheckBox     *m_allowLan = nullptr;
    QLineEdit     *m_protocol = nullptr;
    QLineEdit     *m_address = nullptr;
    QPushButton   *m_copyAddress = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton   *m_clearLog = nullptr;
    QListWidget   *m_clients = nullptr;
    QPushButton   *m_kickButton = nullptr;
    QPlainTextEdit *m_send = nullptr;
    QPushButton   *m_sendButton = nullptr;
    QLabel        *m_status = nullptr;
};

/// 容器：顶部「客户端 / 服务端」切换。
class WebSocketPanel : public QWidget
{
    Q_OBJECT

public:
    explicit WebSocketPanel(QWidget *parent = nullptr);

    /// 供测试/截图用：0 = 客户端，1 = 服务端。
    void showPage(int index);

private:
    QStackedWidget *m_stack = nullptr;
    QPushButton    *m_clientTab = nullptr;
    QPushButton    *m_serverTab = nullptr;
};

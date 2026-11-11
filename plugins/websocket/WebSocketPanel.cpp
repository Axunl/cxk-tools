#include "WebSocketPanel.h"

#include "ThemePalette.h"
#include "LayoutMetrics.h"   // 颜色令牌（宿主与插件共用）

#include "WsProtocol.h"   // 纯协议逻辑（地址解析、子协议解析）
#include "WsServer.h"     // 服务端连接层

#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QTime>
#include <QVBoxLayout>

namespace {

/// 状态行与日志的文字颜色。集中定义，避免各处写字面量后慢慢漂移。
/// 语义令牌而不是色值：日志行是按调用当时的主题着色写进富文本的。
namespace color {
constexpr auto error   = xtools::theme::Color::Danger;      // 握手失败、连接出错
constexpr auto success = xtools::theme::Color::Success;     // 已连接 / 正在监听
constexpr auto hint    = xtools::theme::Color::TextFaint;   // 空闲
constexpr auto warn    = xtools::theme::Color::WarnStrong;  // 连接中、关闭中、没有客户端
constexpr auto recv    = xtools::theme::Color::Text;        // 收到的消息
constexpr auto sent    = xtools::theme::Color::Accent;      // 发出的消息
constexpr auto system  = xtools::theme::Color::TextMuted;   // 协议事件（握手/ping/pong/关闭）
} // namespace color

/// 小节标题，与 JwtPanel / TextKitPanel 同一号灰蓝小字。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    return label;
}

/// 代码/日志区统一外观：等宽字体、描边圆角。
/// 颜色写 @令牌：插件换肤靠宿主遍历控件重算（见 api/ThemePalette.h）。
void styleMonoEdit(QPlainTextEdit *edit)
{
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    xtools::theme::applyStyle(edit, QStringLiteral(
        "QPlainTextEdit{background:@bg;border:1px solid @border;border-radius:8px;"
        "padding:6px 8px;color:@text;}"));
}

/// 客户端列表：与 cronNext / fsTextList 同一套白底细边框观感。
/// 不给 view 本身加 padding（会把 viewport 撑得比内容还小、几行也常驻滚动条）。
void styleClientList(QListWidget *list)
{
    xtools::theme::applyStyle(list, QStringLiteral(
        "QListWidget{background:@bg;border:1px solid @border;border-radius:8px;outline:none;}"
        "QListWidget::item{padding:5px 8px;color:@text-body;border-bottom:1px solid @border-subtle;}"
        "QListWidget::item:last{border-bottom:none;}"
        "QListWidget::item:selected{background:@accent-soft;color:@accent-text;}"));
}

/// 顶部「客户端 / 服务端」分段按钮。
/// 用 checkable + autoExclusive 而不是 QTabWidget：全局 qss 没写 QTabWidget，
/// 默认外观与卡片界面完全不搭，而这里只需要两个互斥按钮。
QPushButton *makeModeTab(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setCheckable(true);
    button->setAutoExclusive(true);   // 同父控件下互斥
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumWidth(84);
    xtools::theme::applyStyle(button, QStringLiteral(
        "QPushButton{border:1px solid @border;background:@bg-alt;color:@text-second;"
        "border-radius:7px;padding:5px 16px;font-weight:600;}"
        "QPushButton:hover{background:@accent-soft;}"
        "QPushButton:checked{background:@accent;border-color:@accent;color:@on-accent;}"));
    return button;
}

/// 追加一行日志：`时:分:秒 + 内容`，按方向/事件上色。
/// 用富文本而不是纯文本，是为了让"发出去"和"收到"在长日志里一眼分得开。
void appendLogLine(QPlainTextEdit *log, const QString &text, xtools::theme::Color token)
{
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    QString body = text.toHtmlEscaped();
    body.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));   // 多行消息保持换行
    // 富文本颜色只能在此刻按令牌取色：已经写进文档的行不会跟着换肤
    // （面板切走就销毁、下次重建是新主题，所以不影响正常使用）。
    log->appendHtml(QStringLiteral("<span style=\"color:%1;\">%2</span>&nbsp;"
                                   "<span style=\"color:%3;\">%4</span>")
                        .arg(xtools::theme::hex(xtools::theme::Color::TextFaint),
                             stamp,
                             xtools::theme::hex(token),
                             body));
}

/// 本机第一个非回环 IPv4。绑定 0.0.0.0 时用它拼「局域网连接地址」，
/// 让手机/同事能照着填；没有网卡（离线）时返回空，由调用方退回 127.0.0.1。
QString localIpv4()
{
    const QList<QHostAddress> addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            return address.toString();
    }
    return QString();
}

/// 会话 id 是 `c12`，日志里显示成 `#12` 更短。
QString shortId(const QString &id)
{
    return QStringLiteral("#") + id.mid(1);
}

} // namespace

// ===========================================================================
// 客户端页
// ===========================================================================

///
/// 一条连接栏 + 请求头 + 消息日志 + 发送框。
///
/// 交互上的几个取舍：
///   * 连接期间锁住地址/子协议/请求头输入框 —— 这些值在握手时已经发出去，
///     改了也不会重连，留着可编辑只会让人以为改了就生效；
///   * 「自动心跳」始终可勾选（连上后切换立即生效），因为它只影响后续 ping；
///   * 发送框不清空：调试时经常要重复发同一条消息，清空反而碍事。
///
/// 所有状态都在面板实例里（宿主可能同时开内嵌与独立窗口两份面板），
/// 不用静态变量共享连接。
///
WsClientPage::WsClientPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 外层 WebSocketPanel 已留 28px 边距，这里不再重复
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    buildConnectRow(layout);
    buildOptionsRow(layout);

    layout->addWidget(makeCaption(QStringLiteral("自定义请求头（可选，每行一个 Key: Value）"), this));
    m_headers = new QPlainTextEdit(this);
    styleMonoEdit(m_headers);
    m_headers->setFixedHeight(56);
    m_headers->setPlaceholderText(QStringLiteral(
        "Authorization: Bearer <token>\n"
        "Cookie: session=..."));
    layout->addWidget(m_headers);

    auto *logRow = new QHBoxLayout;
    logRow->addWidget(makeCaption(QStringLiteral("消息记录"), this));
    logRow->addStretch(1);
    m_clearLog = new QPushButton(QStringLiteral("清空"), this);
    logRow->addWidget(m_clearLog);
    layout->addLayout(logRow);

    m_log = new QPlainTextEdit(this);
    styleMonoEdit(m_log);
    m_log->setReadOnly(true);
    // 只保留最近 600 行：调试长连接时日志会无限增长，超出部分自动丢最旧的
    m_log->setMaximumBlockCount(600);
    m_log->setPlaceholderText(QStringLiteral("连接后这里显示握手结果、收发内容与心跳 / 关闭事件"));
    layout->addWidget(m_log, 1);

    layout->addWidget(makeCaption(QStringLiteral("发送消息（Ctrl+Enter 发送）"), this));
    auto *sendRow = new QHBoxLayout;
    sendRow->setSpacing(8);
    m_send = new QPlainTextEdit(this);
    styleMonoEdit(m_send);
    m_send->setFixedHeight(60);
    m_send->setPlaceholderText(QStringLiteral("要发送的文本，例如 {\"type\":\"ping\"}"));
    sendRow->addWidget(m_send, 1);
    m_sendButton = new QPushButton(QStringLiteral("发送"), this);
    m_sendButton->setProperty("accent", true);
    m_sendButton->setMinimumWidth(76);
    m_sendButton->setEnabled(false);
    sendRow->addWidget(m_sendButton);
    layout->addLayout(sendRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_client = new WsClient(this);
    wireSignals();

    // 初始态不走 applyState()：它会在日志里补一行"未连接"，与下面的提示重复
    setStatus(QStringLiteral("未连接 · 填好地址后点「连接」"), color::hint);
    appendLog(QStringLiteral("提示：ws:// 明文传输，wss:// 走 TLS（默认校验证书）；"
                             "请求头可用来带 Cookie / Authorization"),
              color::system);
}

QWidget *WsClientPage::buildConnectRow(QVBoxLayout *layout)
{
    auto *row = new QWidget(this);
    auto *box = new QHBoxLayout(row);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(8);

    m_url = new QLineEdit(row);
    m_url->setPlaceholderText(QStringLiteral("ws://127.0.0.1:8080/ws   或   wss://example.com/socket"));
    box->addWidget(m_url, 1);

    m_connectButton = new QPushButton(QStringLiteral("连接"), row);
    m_connectButton->setProperty("accent", true);
    m_connectButton->setMinimumWidth(88);
    box->addWidget(m_connectButton);

    layout->addWidget(row);
    return row;
}

QWidget *WsClientPage::buildOptionsRow(QVBoxLayout *layout)
{
    auto *row = new QWidget(this);
    auto *box = new QHBoxLayout(row);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(8);

    box->addWidget(makeCaption(QStringLiteral("子协议"), row));
    m_protocol = new QLineEdit(row);
    m_protocol->setPlaceholderText(QStringLiteral("可选，多个用逗号分隔，如 graphql-ws"));
    box->addWidget(m_protocol, 1);

    layout->addWidget(row);

    // 复选框单独一行：和子协议挤在同一行时，输入框的最小宽度会把「自动心跳」
    // 顶出可视区（截图里被窗口右边裁掉了）。单独一行也不用再抢横向空间。
    auto *toggleRow = new QWidget(this);
    auto *toggleBox = new QHBoxLayout(toggleRow);
    toggleBox->setContentsMargins(0, 0, 0, 0);
    toggleBox->setSpacing(16);

    m_ignoreCert = new QCheckBox(QStringLiteral("跳过证书校验"), toggleRow);
    m_ignoreCert->setToolTip(QStringLiteral("只对 wss:// 生效：自签证书或内网服务连不上时勾选"));
    toggleBox->addWidget(m_ignoreCert);

    m_heartbeat = new QCheckBox(QStringLiteral("自动心跳（每 25 秒 ping，10 秒无 pong 断开）"), toggleRow);
    toggleBox->addWidget(m_heartbeat);
    toggleBox->addStretch(1);

    layout->addWidget(toggleRow);
    return row;
}

/// 把 WsClient 的语义信号接到面板：状态、消息、协议事件、失败。
void WsClientPage::wireSignals()
{
    connect(m_client, &WsClient::stateChanged, this, &WsClientPage::applyState);
    connect(m_client, &WsClient::textReceived, this, [this](const QString &text) {
        appendLog(QStringLiteral("↓ %1").arg(text), color::recv);
    });
    connect(m_client, &WsClient::binaryReceived, this, [this](qint64 bytes) {
        appendLog(QStringLiteral("↓ 二进制消息 %1 字节（本工具只展示文本，内容未展开）").arg(bytes),
                  color::system);
    });
    connect(m_client, &WsClient::protocolNotice, this, [this](const QString &text) {
        appendLog(QStringLiteral("· %1").arg(text), color::system);
    });
    // failure 先于 stateChanged(Idle) 到达：这里记一笔，Idle 分支就不再重复写"已断开"
    connect(m_client, &WsClient::failure, this, [this](const QString &reason) {
        m_failureLogged = true;
        appendLog(QStringLiteral("✕ %1").arg(reason), color::error);
    });

    connect(m_connectButton, &QPushButton::clicked, this, &WsClientPage::toggleConnection);
    connect(m_url, &QLineEdit::returnPressed, this, &WsClientPage::toggleConnection);
    connect(m_sendButton, &QPushButton::clicked, this, &WsClientPage::sendCurrent);
    connect(m_clearLog, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    // 心跳勾选立即生效（没连上时只记下意愿，连上后 open() 里按它启动）
    connect(m_heartbeat, &QCheckBox::toggled, m_client, &WsClient::setHeartbeatEnabled);

    // Ctrl+Enter 发送：作用域限定在本页的子控件，不会抢到宿主主窗口
    for (const char *sequence : {"Ctrl+Return", "Ctrl+Enter"}) {
        auto *shortcut = new QShortcut(QKeySequence(QString::fromLatin1(sequence)), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, &WsClientPage::sendCurrent);
    }
}

void WsClientPage::toggleConnection()
{
    if (m_client->state() == WsClient::State::Idle)
        startConnection();
    else
        m_client->close();
}

void WsClientPage::startConnection()
{
    const wsproto::Url url = wsproto::parseUrl(m_url->text());
    if (!url.valid) {
        setStatus(url.error, color::error);
        appendLog(QStringLiteral("✕ %1").arg(url.error), color::error);
        m_url->setFocus();
        return;
    }

    // sanitizeHeaders / parseProtocols 在纯逻辑层做完校验，这里只负责取结果
    m_client->open(url,
                   wsproto::parseProtocols(m_protocol->text()),
                   wsproto::sanitizeHeaders(m_headers->toPlainText()),
                   m_ignoreCert->isChecked());
}

void WsClientPage::sendCurrent()
{
    if (m_client->state() != WsClient::State::Open)
        return;

    const QString text = m_send->toPlainText();
    if (text.isEmpty()) {
        setStatus(QStringLiteral("发送框是空的"), color::hint);
        m_send->setFocus();
        return;
    }
    if (!m_client->sendText(text)) {
        appendLog(QStringLiteral("✕ 发送失败：连接已断开"), color::error);
        return;
    }
    appendLog(QStringLiteral("↑ %1").arg(text), color::sent);
}

/// 连接状态 → 按钮文案 / 输入可用性 / 状态行 / 日志。所有状态收敛在这一个函数里。
void WsClientPage::applyState(WsClient::State state, const QString &detail)
{
    switch (state) {
    case WsClient::State::Connecting:
        m_connectButton->setText(QStringLiteral("取消"));
        m_connectButton->setEnabled(true);
        setConfigEnabled(false);
        m_sendButton->setEnabled(false);
        setStatus(detail, color::warn);
        break;

    case WsClient::State::Open:
        m_connectButton->setText(QStringLiteral("断开"));
        m_connectButton->setEnabled(true);
        setConfigEnabled(false);
        m_sendButton->setEnabled(true);
        setStatus(QStringLiteral("%1 · 可以在下方发送文本").arg(detail), color::success);
        appendLog(QStringLiteral("✓ %1：%2").arg(detail, m_url->text().trimmed()),
                  color::success);
        break;

    case WsClient::State::Closing:
        m_connectButton->setText(QStringLiteral("断开中…"));
        m_connectButton->setEnabled(false);   // 避免重复发关闭帧
        m_sendButton->setEnabled(false);
        setStatus(detail, color::warn);
        break;

    case WsClient::State::Idle:
    default:
        m_connectButton->setText(QStringLiteral("连接"));
        m_connectButton->setEnabled(true);
        setConfigEnabled(true);
        m_sendButton->setEnabled(false);
        setStatus(detail, m_failureLogged ? color::error : color::hint);
        if (!m_failureLogged)
            appendLog(QStringLiteral("· %1").arg(detail), color::system);
        m_failureLogged = false;
        break;
    }
}

/// 连接期间锁住握手相关的输入框（值已经发出去了，改也不会重连）。
void WsClientPage::setConfigEnabled(bool enabled)
{
    m_url->setEnabled(enabled);
    m_protocol->setEnabled(enabled);
    m_headers->setEnabled(enabled);
    m_ignoreCert->setEnabled(enabled);
}

void WsClientPage::setStatus(const QString &text, xtools::theme::Color token)
{
    xtools::theme::applyStyle(m_status, QStringLiteral("font-size:12px;color:@%1;")
                                            .arg(xtools::theme::tokenName(token)));
    m_status->setText(text);
}

void WsClientPage::appendLog(const QString &text, xtools::theme::Color token)
{
    appendLogLine(m_log, text, token);
}

// ===========================================================================
// 服务端页
// ===========================================================================

///
/// 监听设置 + 客户端列表 + 消息日志 + 发送框。
///
/// 交互约定：
///   * 默认只绑 127.0.0.1：起服务端是"本机调试"，对外暴露必须是用户主动勾选；
///   * 客户端列表里**不选任何一项就是群发**（调试时最常用），选中的就是定向发送；
///   * 子协议留空 = 回显客户端提出的第一个，这样任何客户端都能连上；
///   * 关闭面板即停止监听（宿主不缓存面板），页面上写明了这一点。
///
WsServerPage::WsServerPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    buildListenRows(layout);

    auto *middle = new QHBoxLayout;
    middle->setSpacing(10);

    auto *leftCol = new QVBoxLayout;
    leftCol->setSpacing(4);
    auto *logRow = new QHBoxLayout;
    logRow->addWidget(makeCaption(QStringLiteral("消息记录"), this));
    logRow->addStretch(1);
    m_clearLog = new QPushButton(QStringLiteral("清空"), this);
    logRow->addWidget(m_clearLog);
    leftCol->addLayout(logRow);

    m_log = new QPlainTextEdit(this);
    styleMonoEdit(m_log);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(600);
    m_log->setPlaceholderText(QStringLiteral("启动服务后这里显示握手结果、各客户端的收发内容与断开原因"));
    leftCol->addWidget(m_log, 1);
    middle->addLayout(leftCol, 1);

    auto *rightCol = new QVBoxLayout;
    rightCol->setSpacing(4);
    rightCol->addWidget(makeCaption(QStringLiteral("客户端（不选 = 群发）"), this));
    m_clients = new QListWidget(this);
    m_clients->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_clients->setFixedWidth(188);
    styleClientList(m_clients);
    rightCol->addWidget(m_clients, 1);
    m_kickButton = new QPushButton(QStringLiteral("断开选中"), this);
    m_kickButton->setEnabled(false);
    rightCol->addWidget(m_kickButton);
    middle->addLayout(rightCol);

    layout->addLayout(middle, 1);

    layout->addWidget(makeCaption(QStringLiteral("发送消息（Ctrl+Enter 发送；选中客户端定向发，不选则群发）"), this));
    auto *sendRow = new QHBoxLayout;
    sendRow->setSpacing(8);
    m_send = new QPlainTextEdit(this);
    styleMonoEdit(m_send);
    m_send->setFixedHeight(60);
    m_send->setPlaceholderText(QStringLiteral("要推送给客户端的文本，例如 {\"type\":\"hello\"}"));
    sendRow->addWidget(m_send, 1);
    m_sendButton = new QPushButton(QStringLiteral("发送"), this);
    m_sendButton->setProperty("accent", true);
    m_sendButton->setMinimumWidth(76);
    m_sendButton->setEnabled(false);
    sendRow->addWidget(m_sendButton);
    layout->addLayout(sendRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_server = new WsServer(this);
    wireSignals();

    setConfigEnabled(true);
    setStatus(QStringLiteral("未启动 · 填好端口后点「启动服务」，客户端连 ws://127.0.0.1:%1/")
                  .arg(m_port->text()),
              color::hint);
    appendLog(QStringLiteral("提示：这是本机 ws:// 服务端，浏览器控制台 new WebSocket(...) 就能连；"
                             "勾选「允许局域网访问」后手机等设备也能连"),
              color::system);
}

QWidget *WsServerPage::buildListenRows(QVBoxLayout *layout)
{
    auto *row = new QWidget(this);
    auto *box = new QHBoxLayout(row);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(8);

    box->addWidget(makeCaption(QStringLiteral("端口"), row));
    m_port = new QLineEdit(row);
    m_port->setText(QStringLiteral("8765"));
    m_port->setFixedWidth(80);
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    box->addWidget(m_port);

    m_listenButton = new QPushButton(QStringLiteral("启动服务"), row);
    m_listenButton->setProperty("accent", true);
    m_listenButton->setMinimumWidth(96);
    box->addWidget(m_listenButton);

    m_allowLan = new QCheckBox(QStringLiteral("允许局域网访问"), row);
    m_allowLan->setToolTip(QStringLiteral("默认只绑 127.0.0.1（仅本机可连）；"
                                          "勾选后绑 0.0.0.0，同一网段的手机/电脑也能连进来"));
    box->addWidget(m_allowLan);
    box->addSpacing(8);   // 与左侧复选框拉开距离，否则「连接地址」贴着复选框

    box->addWidget(makeCaption(QStringLiteral("连接地址"), row));
    m_address = new QLineEdit(row);
    m_address->setReadOnly(true);
    m_address->setPlaceholderText(QStringLiteral("启动后显示可复制的 ws:// 地址"));
    box->addWidget(m_address, 1);

    m_copyAddress = new QPushButton(QStringLiteral("复制"), row);
    m_copyAddress->setMinimumWidth(60);
    m_copyAddress->setEnabled(false);
    box->addWidget(m_copyAddress);

    layout->addWidget(row);

    auto *protocolRow = new QWidget(this);
    auto *protocolBox = new QHBoxLayout(protocolRow);
    protocolBox->setContentsMargins(0, 0, 0, 0);
    protocolBox->setSpacing(8);
    protocolBox->addWidget(makeCaption(QStringLiteral("子协议"), protocolRow));
    m_protocol = new QLineEdit(protocolRow);
    m_protocol->setPlaceholderText(QStringLiteral("可选：留空则回显客户端提出的第一个子协议；填了则只接受列表里的"));
    protocolBox->addWidget(m_protocol, 1);
    layout->addWidget(protocolRow);

    return row;
}

void WsServerPage::wireSignals()
{
    connect(m_listenButton, &QPushButton::clicked, this, &WsServerPage::toggleListening);
    connect(m_port, &QLineEdit::returnPressed, this, &WsServerPage::toggleListening);
    connect(m_copyAddress, &QPushButton::clicked, this, &WsServerPage::copyAddress);
    connect(m_sendButton, &QPushButton::clicked, this, &WsServerPage::sendCurrent);
    connect(m_clearLog, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    connect(m_kickButton, &QPushButton::clicked, this, [this] {
        const QStringList ids = selectedIds();
        if (ids.isEmpty())
            return;
        for (const QString &id : ids)
            m_server->disconnectClient(id, 1000, QStringLiteral("服务端主动断开"));
    });
    // 列表选中状态变化只影响「断开选中」按钮；发送按钮行为不变（不选即群发）
    connect(m_clients, &QListWidget::itemSelectionChanged, this, [this] {
        m_kickButton->setEnabled(!selectedIds().isEmpty());
    });

    connect(m_server, &WsServer::listening, this, &WsServerPage::onListening);
    connect(m_server, &WsServer::stopped, this, &WsServerPage::onStopped);
    connect(m_server, &WsServer::failure, this, &WsServerPage::onFailure);
    connect(m_server, &WsServer::clientConnected, this,
            [this](const QString &id, const QString &peer, const QString &protocol) {
                QString detail = QStringLiteral("✓ 客户端 %1 已连接（%2）").arg(shortId(id), peer);
                if (!protocol.isEmpty())
                    detail += QStringLiteral("，子协议 %1").arg(protocol);
                appendLog(detail, color::success);
                refreshClients();
            });
    connect(m_server, &WsServer::clientDisconnected, this,
            [this](const QString &id, const QString &reason) {
                appendLog(QStringLiteral("· 客户端 %1 已断开：%2").arg(shortId(id), reason),
                          color::system);
                refreshClients();
            });
    connect(m_server, &WsServer::textReceived, this,
            [this](const QString &id, const QString &text) {
                appendLog(QStringLiteral("↓ [%1] %2").arg(shortId(id), text), color::recv);
            });
    connect(m_server, &WsServer::binaryReceived, this,
            [this](const QString &id, qint64 bytes) {
                appendLog(QStringLiteral("↓ [%1] 二进制消息 %2 字节（只展示大小）")
                              .arg(shortId(id))
                              .arg(bytes),
                          color::system);
            });
    connect(m_server, &WsServer::protocolNotice, this,
            [this](const QString &id, const QString &text) {
                appendLog(QStringLiteral("· [%1] %2").arg(shortId(id), text), color::system);
            });

    for (const char *sequence : {"Ctrl+Return", "Ctrl+Enter"}) {
        auto *shortcut = new QShortcut(QKeySequence(QString::fromLatin1(sequence)), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, &WsServerPage::sendCurrent);
    }
}

void WsServerPage::toggleListening()
{
    if (m_server->isListening()) {
        m_server->stop(QStringLiteral("已停止监听"));
        return;
    }

    bool ok = false;
    const int port = m_port->text().trimmed().toInt(&ok);
    if (!ok || port < 1 || port > 65535) {
        setStatus(QStringLiteral("端口不合法：请填 1–65535 之间的整数"), color::error);
        m_port->setFocus();
        return;
    }

    // 默认只绑回环地址：对外暴露必须是用户主动勾选「允许局域网访问」
    const QHostAddress address = m_allowLan->isChecked()
                                     ? QHostAddress(QHostAddress::AnyIPv4)
                                     : QHostAddress(QHostAddress::LocalHost);
    m_server->listen(address, static_cast<quint16>(port),
                     wsproto::parseProtocols(m_protocol->text()));
}

void WsServerPage::onListening(quint16 port)
{
    // 端口可能被系统分配（用户填 0 时）或被监听改写，回填实际值
    m_port->setText(QString::number(port));
    m_listenButton->setText(QStringLiteral("停止服务"));
    setConfigEnabled(false);
    m_sendButton->setEnabled(true);
    m_address->setText(connectUrl(port));
    m_copyAddress->setEnabled(true);
    updateStatusSummary();
    appendLog(QStringLiteral("✓ 服务端已启动，客户端连 %1（关闭本面板即停止监听）")
                  .arg(connectUrl(port)),
              color::success);
}

void WsServerPage::onStopped(const QString &reason)
{
    m_listenButton->setText(QStringLiteral("启动服务"));
    setConfigEnabled(true);
    m_sendButton->setEnabled(false);
    m_address->clear();
    m_copyAddress->setEnabled(false);
    m_clients->clear();
    m_kickButton->setEnabled(false);
    setStatus(reason, color::hint);
    appendLog(QStringLiteral("· %1").arg(reason), color::system);
}

void WsServerPage::onFailure(const QString &reason)
{
    setStatus(reason, color::error);
    appendLog(QStringLiteral("✕ %1").arg(reason), color::error);
    // 监听失败时按钮必须回到"启动服务"，否则用户以为还在监听
    m_listenButton->setText(QStringLiteral("启动服务"));
    setConfigEnabled(true);
    m_sendButton->setEnabled(false);
    m_address->clear();
    m_copyAddress->setEnabled(false);
}

void WsServerPage::refreshClients()
{
    const QStringList keep = selectedIds();
    m_clients->clear();
    const QList<WsServer::Client> clients = m_server->clients();
    for (const WsServer::Client &client : clients) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 %2").arg(shortId(client.id), client.peer), m_clients);
        item->setData(Qt::UserRole, client.id);
        QString tip = QStringLiteral("路径 %1").arg(client.target);
        if (!client.protocol.isEmpty())
            tip += QStringLiteral("，子协议 %1").arg(client.protocol);
        item->setToolTip(tip);
        item->setSelected(keep.contains(client.id));
    }
    m_kickButton->setEnabled(!selectedIds().isEmpty());
    updateStatusSummary();
}

void WsServerPage::sendCurrent()
{
    if (!m_server->isListening()) {
        setStatus(QStringLiteral("服务端没在监听：先点「启动服务」"), color::warn);
        return;
    }
    const QString text = m_send->toPlainText();
    if (text.isEmpty()) {
        setStatus(QStringLiteral("发送框是空的"), color::hint);
        m_send->setFocus();
        return;
    }

    const QStringList ids = selectedIds();
    if (ids.isEmpty()) {
        const int sent = m_server->broadcastText(text);
        if (sent == 0) {
            setStatus(QStringLiteral("还没有客户端连上来，消息没有发出去"), color::warn);
            return;
        }
        appendLog(QStringLiteral("↑ 群发给 %1 个客户端：%2").arg(sent).arg(text), color::sent);
        return;
    }

    QStringList done;
    for (const QString &id : ids) {
        if (m_server->sendText(id, text))
            done << shortId(id);
    }
    if (done.isEmpty()) {
        appendLog(QStringLiteral("✕ 发送失败：选中的客户端都已断开"), color::error);
        return;
    }
    appendLog(QStringLiteral("↑ 发给 %1：%2").arg(done.join(QStringLiteral("、")), text), color::sent);
}

void WsServerPage::copyAddress()
{
    const QString url = m_address->text();
    if (url.isEmpty())
        return;
    // 回读校验：QClipboard::setText() 没有返回值，Windows 上剪贴板被别的进程
    // 占用时 Qt 只打一条 COM 警告，不回读就会把失败当成功报给用户。
    QGuiApplication::clipboard()->setText(url);
    const bool ok = QGuiApplication::clipboard()->text() == url;
    appendLog(ok ? QStringLiteral("· 已复制连接地址：%1").arg(url)
                 : QStringLiteral("✕ 复制失败：剪贴板被其他程序占用"),
              ok ? color::system : color::error);
}

QStringList WsServerPage::selectedIds() const
{
    QStringList ids;
    const QList<QListWidgetItem *> items = m_clients->selectedItems();
    for (const QListWidgetItem *item : items)
        ids << item->data(Qt::UserRole).toString();
    return ids;
}

void WsServerPage::setConfigEnabled(bool enabled)
{
    m_port->setEnabled(enabled);
    m_allowLan->setEnabled(enabled);
    m_protocol->setEnabled(enabled);
}

void WsServerPage::setStatus(const QString &text, xtools::theme::Color token)
{
    xtools::theme::applyStyle(m_status, QStringLiteral("font-size:12px;color:@%1;")
                                            .arg(xtools::theme::tokenName(token)));
    m_status->setText(text);
}

void WsServerPage::updateStatusSummary()
{
    if (!m_server->isListening())
        return;
    const QString bindAddress = m_allowLan->isChecked() ? QStringLiteral("0.0.0.0")
                                                       : QStringLiteral("127.0.0.1");
    setStatus(QStringLiteral("正在监听 %1:%2 · %3 个客户端 · 连接地址 %4")
                  .arg(bindAddress)
                  .arg(m_server->port())
                  .arg(m_server->clientCount())
                  .arg(connectUrl(m_server->port())),
              color::success);
}

void WsServerPage::appendLog(const QString &text, xtools::theme::Color token)
{
    appendLogLine(m_log, text, token);
}

QString WsServerPage::connectUrl(quint16 port) const
{
    // 勾了局域网就显示本机在局域网里的地址（手机照着填即可）；
    // 没勾就用 127.0.0.1 —— 只绑回环时局域网 IP 是连不上的，显示它只会误导。
    QString host = QStringLiteral("127.0.0.1");
    if (m_allowLan->isChecked()) {
        const QString lan = localIpv4();
        if (!lan.isEmpty())
            host = lan;
    }
    return QStringLiteral("ws://%1:%2/").arg(host).arg(port);
}

// ===========================================================================
// 容器
// ===========================================================================

WebSocketPanel::WebSocketPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐
    layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    layout->setSpacing(10);

    // 分段开关必须同父（autoExclusive 只在同一父控件的兄弟间互斥）
    auto *tabHost = new QWidget(this);
    auto *tabRow = new QHBoxLayout(tabHost);
    tabRow->setContentsMargins(0, 0, 0, 0);
    tabRow->setSpacing(6);

    m_clientTab = makeModeTab(QStringLiteral("客户端"), tabHost);
    m_serverTab = makeModeTab(QStringLiteral("服务端"), tabHost);
    m_clientTab->setChecked(true);
    tabRow->addWidget(m_clientTab);
    tabRow->addWidget(m_serverTab);
    tabRow->addStretch(1);
    layout->addWidget(tabHost);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(new WsClientPage(m_stack));
    m_stack->addWidget(new WsServerPage(m_stack));
    layout->addWidget(m_stack, 1);

    connect(m_clientTab, &QPushButton::clicked, this, [this] { showPage(0); });
    connect(m_serverTab, &QPushButton::clicked, this, [this] { showPage(1); });
}

void WebSocketPanel::showPage(int index)
{
    if (m_stack == nullptr || index < 0 || index >= m_stack->count())
        return;
    m_stack->setCurrentIndex(index);
    m_clientTab->setChecked(index == 0);
    m_serverTab->setChecked(index == 1);
}

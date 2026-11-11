#include "FinancePanel.h"

#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用，换肤时宿主会重算）

#include <QButtonGroup>
#include <QClipboard>
#include <QDir>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace {

/// 数值列的固定宽度。三列都定宽，行与列头才能对得整整齐齐
/// （用 QGridLayout 也能对齐，但行高回收、悬停底色会麻烦得多）。
constexpr int kColPrice = 104;
constexpr int kColPercent = 92;
constexpr int kColChange = 92;
constexpr int kColAction = 26;

/// 自动刷新间隔。行情类工具停在页面上看的时候多，20 秒足够新鲜，
/// 又不会把公开接口打得太频繁。
constexpr int kAutoRefreshMs = 20 * 1000;

/// 自选落盘位置：放在宿主配置目录下的独立 ini，不跟主程序的 cxk-tools.ini 混在一起。
/// 为什么不复用宿主的 Settings 单例：那是主程序的内部类，插件看不到（只链 xtools_api）。
/// 用 QSettings(filePath, IniFormat) 而不是默认构造，是为了三平台都落到同一个文件，
/// 避免 Windows 写注册表、Linux 写 ~/.config 的两套行为。
QString settingsFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/finance.ini");
}

/// 行首的说明文字："上海金交所 · AU9999" / "场外基金 · 000001" / "沪市 · 600519"。
QString symbolSubtitle(const finance::Symbol &symbol)
{
    if (finance::isFundId(symbol.id))
        return QStringLiteral("场外基金 · %1").arg(finance::fundCodeOf(symbol.id));

    const int dot = symbol.id.indexOf(QLatin1Char('.'));
    const QString code = dot >= 0 ? symbol.id.mid(dot + 1) : symbol.id;
    const QString market = finance::marketLabel(symbol.id);
    return market.isEmpty() ? code : QStringLiteral("%1 · %2").arg(market, code);
}

/// 各分组的预设品种。名字只是兜底文案：行情回包里有真名，拿到就覆盖
/// （基金改名、指数更名都很常见，不要在本地硬编码当真名用）。
QVector<finance::Symbol> presetSymbols(int tab)
{
    using finance::Kind;
    switch (tab) {
    case 1:   // 黄金
        return {
            {QStringLiteral("118.AU9999"), QStringLiteral("黄金9999"), Kind::Other},
            {QStringLiteral("113.aum"), QStringLiteral("沪金主连"), Kind::Other},
            {QStringLiteral("101.GC00Y"), QStringLiteral("COMEX黄金"), Kind::Other},
            {QStringLiteral("101.SI00Y"), QStringLiteral("COMEX白银"), Kind::Other},
        };
    case 2:   // 场外基金
        return {
            {QStringLiteral("fund:000001"), QStringLiteral("华夏成长混合"), Kind::Fund},
            {QStringLiteral("fund:110022"), QStringLiteral("易方达消费行业股票"), Kind::Fund},
            {QStringLiteral("fund:005827"), QStringLiteral("易方达蓝筹精选混合"), Kind::Fund},
            {QStringLiteral("fund:161725"), QStringLiteral("招商中证白酒指数"), Kind::Fund},
        };
    case 3:   // 股票与指数
        return {
            {QStringLiteral("1.000001"), QStringLiteral("上证指数"), Kind::Index},
            {QStringLiteral("0.399001"), QStringLiteral("深证成指"), Kind::Index},
            {QStringLiteral("0.399006"), QStringLiteral("创业板指"), Kind::Index},
            {QStringLiteral("1.000300"), QStringLiteral("沪深300"), Kind::Index},
            {QStringLiteral("1.600519"), QStringLiteral("贵州茅台"), Kind::Stock},
            {QStringLiteral("0.300750"), QStringLiteral("宁德时代"), Kind::Stock},
            {QStringLiteral("1.601318"), QStringLiteral("中国平安"), Kind::Stock},
        };
    default:
        break;
    }
    return {};
}

/// 首次运行的自选种子。空列表当第一屏太像"坏了"，给三条能立刻看懂的数据，
/// 用户删掉即可（删空后不会再次填充：只有 ini 里完全没有 watchlist 组时才种）。
QVector<finance::Symbol> defaultWatchlist()
{
    return {
        {QStringLiteral("118.AU9999"), QStringLiteral("黄金9999"), finance::Kind::Other},
        {QStringLiteral("1.510300"), QStringLiteral("沪深300ETF"), finance::Kind::Etf},
        {QStringLiteral("fund:000001"), QStringLiteral("华夏成长混合"), finance::Kind::Fund},
    };
}

} // namespace

/// 列表区的一行：左侧名称 + 代码，右侧三列数值，末尾可选删除按钮。
///
/// 刻意不带 Q_OBJECT：没有信号槽需求，点击与删除都用 std::function 回调，
/// 省掉 moc 也省掉「面板销毁后回调打到野指针」的可能（回调随行对象一起销毁）。
///
/// 注意它**不在匿名命名空间里**：FinancePanel.h 用前置声明持有它的指针，
/// 匿名命名空间里的同名类会是另一个类型（编译期报「不是 finance 的成员」）。
class FinanceQuoteRow : public QFrame
{
public:
    explicit FinanceQuoteRow(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("finRow"));
        // QWidget 子类默认不画样式背景，不设这个属性 hover 底色不生效
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 9, 12, 9);
        layout->setSpacing(8);

        auto *textCol = new QVBoxLayout;
        textCol->setSpacing(2);

        m_name = new QLabel(this);
        xtools::theme::applyStyle(m_name, QStringLiteral(
            "font-size:14px;font-weight:600;color:@text;background:transparent;"));
        textCol->addWidget(m_name);

        m_sub = new QLabel(this);
        xtools::theme::applyStyle(m_sub, QStringLiteral(
            "font-size:12px;color:@text-faint;background:transparent;"));
        textCol->addWidget(m_sub);

        layout->addLayout(textCol, 1);

        m_price = makeColumn(kColPrice, 15, true);
        m_percent = makeColumn(kColPercent, 14, true);
        m_change = makeColumn(kColChange, 12, false);
        layout->addWidget(m_price);
        layout->addWidget(m_percent);
        layout->addWidget(m_change);

        m_action = new QPushButton(QStringLiteral("✕"), this);
        m_action->setObjectName(QStringLiteral("finRowDel"));
        // 全局 QPushButton 有 padding: 6px 14px，不清掉这颗 20px 的小按钮会被挤成一条
        m_action->setFixedSize(20, 20);
        m_action->setCursor(Qt::PointingHandCursor);
        m_action->setToolTip(QStringLiteral("从自选中移除"));
        m_action->hide();
        connect(m_action, &QPushButton::clicked, this, [this] {
            if (m_onDelete)
                m_onDelete();
        });
        layout->addWidget(m_action, 0, Qt::AlignVCenter);
    }

    /// 品种身份：名称、说明行在这里定型，报价稍后异步补上。
    void setSymbol(const finance::Symbol &symbol)
    {
        m_symbol = symbol;
        m_name->setText(symbol.name);
        m_sub->setText(symbolSubtitle(symbol));
    }

    void setQuote(const finance::Quote &quote)
    {
        if (!quote.name.isEmpty() && quote.name != m_symbol.name) {
            m_symbol.name = quote.name;   // 回包里的名字更新，以接口为准
            m_name->setText(quote.name);
        }

        const QString token = quote.hasPrice
            ? (finance::direction(quote.changePercent) > 0
                   ? QStringLiteral("quote-up")
                   : finance::direction(quote.changePercent) < 0
                         ? QStringLiteral("quote-down")
                         : QStringLiteral("quote-flat"))
            : QStringLiteral("quote-flat");

        if (quote.hasPrice) {
            m_price->setText(finance::formatPrice(quote.price, quote.decimals));
            m_percent->setText(finance::formatPercent(quote.changePercent));
            m_change->setText(finance::formatSigned(quote.change, quote.decimals));
        } else {
            // 停牌 / 未开盘 / 接口没给数：三列统一显示破折号，
            // 不要显示 0.00 —— 那看起来像「跌到零」。
            m_price->setText(QStringLiteral("—"));
            m_percent->setText(QStringLiteral("—"));
            m_change->setText(QStringLiteral("—"));
        }

        xtools::theme::applyStyle(m_price, QStringLiteral(
            "font-size:15px;font-weight:600;color:@%1;background:transparent;").arg(token));
        xtools::theme::applyStyle(m_percent, QStringLiteral(
            "font-size:14px;font-weight:600;color:@%1;background:transparent;").arg(token));
        xtools::theme::applyStyle(m_change, QStringLiteral(
            "font-size:12px;color:@%1;background:transparent;").arg(token));

        const QString subtitle = symbolSubtitle(m_symbol);
        m_sub->setText(quote.note.isEmpty() ? subtitle
                                            : QStringLiteral("%1 · %2").arg(subtitle, quote.note));

        m_copyText = QStringLiteral("%1 %2 %3")
                         .arg(m_symbol.name,
                              finance::formatPrice(quote.price, quote.decimals),
                              finance::formatPercent(quote.changePercent));
        m_hasData = quote.hasPrice;
    }

    void setDeletable(bool deletable)
    {
        m_action->setVisible(deletable);
    }

    void setOnDelete(std::function<void()> callback)
    {
        m_onDelete = std::move(callback);
    }

    void setOnActivate(std::function<void()> callback)
    {
        m_onActivate = std::move(callback);
    }

    QString copyText() const { return m_copyText; }
    bool hasData() const { return m_hasData; }

protected:
    /// 整行任意位置（含压在上面的子控件）点击都算激活：子 QLabel 的鼠标事件
    /// 会冒泡到这里，所以不用给每个 label 单独装事件过滤器。
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_onActivate)
            m_onActivate();
        QFrame::mouseReleaseEvent(event);
    }

private:
    /// 数值列：固定宽度 + 右对齐，行与列头才对得齐。
    /// 注意第一个参数是「列宽」不是字号 —— 两者都在这几行里，别再看串。
    QLabel *makeColumn(int width, int fontSize, bool bold)
    {
        auto *label = new QLabel(this);
        label->setFixedWidth(width);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        xtools::theme::applyStyle(label, QStringLiteral(
            "font-size:%1px;%2color:@text-body;background:transparent;")
            .arg(fontSize)
            .arg(bold ? QStringLiteral("font-weight:600;") : QString()));
        return label;
    }

    finance::Symbol m_symbol;
    QLabel *m_name = nullptr;
    QLabel *m_sub = nullptr;
    QLabel *m_price = nullptr;
    QLabel *m_percent = nullptr;
    QLabel *m_change = nullptr;
    QPushButton *m_action = nullptr;
    std::function<void()> m_onDelete;
    std::function<void()> m_onActivate;
    QString m_copyText;
    bool m_hasData = false;
};

namespace {

/// 搜索结果行：名称 + 代码 + 品种标签，点击加入自选。
/// 只在 .cpp 内部使用，放进匿名命名空间（不需要被头文件引用）。
class SearchResultRow : public QFrame
{
public:
    SearchResultRow(const finance::Symbol &symbol, std::function<void()> onActivate,
                    QWidget *parent = nullptr)
        : QFrame(parent), m_onActivate(std::move(onActivate))
    {
        setObjectName(QStringLiteral("finSearchRow"));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 7, 10, 7);
        layout->setSpacing(8);

        auto *name = new QLabel(symbol.name, this);
        xtools::theme::applyStyle(name, QStringLiteral(
            "font-size:13px;font-weight:600;color:@text;background:transparent;"));
        layout->addWidget(name);

        auto *code = new QLabel(symbolSubtitle(symbol), this);
        xtools::theme::applyStyle(code, QStringLiteral(
            "font-size:12px;color:@text-faint;background:transparent;"));
        layout->addWidget(code);

        layout->addStretch(1);

        auto *kind = new QLabel(finance::kindLabel(symbol.kind), this);
        xtools::theme::applyStyle(kind, QStringLiteral(
            "font-size:11px;color:@accent-text;background:@accent-soft;"
            "border-radius:7px;padding:1px 7px;"));
        layout->addWidget(kind);
    }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_onActivate)
            m_onActivate();
        QFrame::mouseReleaseEvent(event);
    }

private:
    std::function<void()> m_onActivate;
};

} // namespace

// ---------------------------------------------------------------- 构造 / 布局

FinancePanel::FinancePanel(QWidget *parent)
    : QWidget(parent)
{
    reloadWatchlist();
    buildUi();

    m_autoRefresh = new QTimer(this);
    m_autoRefresh->setInterval(kAutoRefreshMs);
    connect(m_autoRefresh, &QTimer::timeout, this, &FinancePanel::refresh);

    // F5 刷新（WidgetWithChildrenShortcut：只在本面板内生效，
    // 不跟主窗口/独立窗口的其他快捷键抢）
    auto *refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
    refreshShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(refreshShortcut, &QShortcut::activated, this, &FinancePanel::refresh);

    rebuildRows();
}

FinancePanel::~FinancePanel() = default;

void FinancePanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐（同 CalcPanel / IpQueryPanel 的约定）
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(10);

    // ---- 顶部工具条：分组标签 + 搜索框 + 刷新 ----
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);

    m_tabs = new QButtonGroup(this);
    m_tabs->setExclusive(true);
    const QStringList tabNames{QStringLiteral("自选"), QStringLiteral("黄金"),
                               QStringLiteral("基金"), QStringLiteral("股票")};
    for (int i = 0; i < TabCount; ++i) {
        auto *button = new QPushButton(tabNames.at(i), this);
        button->setObjectName(QStringLiteral("finTab"));
        button->setCheckable(true);
        button->setChecked(i == TabWatch);
        button->setCursor(Qt::PointingHandCursor);
        m_tabs->addButton(button, i);
        toolbar->addWidget(button);
    }
    connect(m_tabs, &QButtonGroup::idClicked, this, &FinancePanel::setTab);

    toolbar->addStretch(1);

    m_addEdit = new QLineEdit(this);
    m_addEdit->setPlaceholderText(QStringLiteral("输入代码或名称，回车加入自选"));
    m_addEdit->setClearButtonEnabled(true);
    m_addEdit->setMinimumWidth(196);
    m_addEdit->setMaximumWidth(260);
    connect(m_addEdit, &QLineEdit::returnPressed, this, &FinancePanel::startSearch);
    toolbar->addWidget(m_addEdit);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setObjectName(QStringLiteral("finRefresh"));
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_refreshButton->setToolTip(QStringLiteral("重新获取当前分组的行情（F5）"));
    connect(m_refreshButton, &QPushButton::clicked, this, &FinancePanel::refresh);
    toolbar->addWidget(m_refreshButton);

    layout->addLayout(toolbar);

    // ---- 列头 ----
    auto *header = new QHBoxLayout;
    header->setContentsMargins(12, 0, 12, 0);
    header->setSpacing(8);

    auto *nameHead = new QLabel(QStringLiteral("名称 / 代码"), this);
    xtools::theme::applyStyle(nameHead, QStringLiteral(
        "font-size:12px;color:@text-faint;background:transparent;"));
    header->addWidget(nameHead, 1);
    header->addWidget(makeHeaderLabel(QStringLiteral("最新价"), kColPrice));
    header->addWidget(makeHeaderLabel(QStringLiteral("涨跌幅"), kColPercent));
    header->addWidget(makeHeaderLabel(QStringLiteral("涨跌额"), kColChange));
    header->addSpacing(kColAction);
    layout->addLayout(header);

    // ---- 搜索结果区（默认隐藏，回车搜索后才出现） ----
    m_searchBox = new QFrame(this);
    m_searchBox->setObjectName(QStringLiteral("finSearchBox"));
    m_searchLayout = new QVBoxLayout(m_searchBox);
    m_searchLayout->setContentsMargins(8, 8, 8, 8);
    m_searchLayout->setSpacing(2);
    m_searchBox->hide();
    layout->addWidget(m_searchBox);

    // ---- 报价列表 ----
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    xtools::theme::applyStyle(scroll, QStringLiteral(
        "QScrollArea{background:transparent;border:none;}"));

    m_rowsHost = new QWidget(scroll);
    m_rowsLayout = new QVBoxLayout(m_rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    // 行本身有 9px 内边距，spacing 再大就会散架
    m_rowsLayout->setSpacing(2);
    m_rowsLayout->addStretch();   // 行贴顶排列，不留空档
    scroll->setWidget(m_rowsHost);
    layout->addWidget(scroll, 1);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    // 整块面板的局部样式。颜色全走令牌：宿主换肤时会遍历控件内联样式重算，
    // 所以切深色不需要重启，也不会留一块白底（见 AGENTS.md 规则 7）。
    xtools::theme::applyStyle(this, QStringLiteral(
        "QPushButton#finTab{border:none;background:transparent;color:@text-muted;"
        "padding:6px 12px;border-radius:8px;font-size:13px;}"
        "QPushButton#finTab:hover{background:@bg-subtle;color:@text;}"
        "QPushButton#finTab:checked{background:@accent-soft;color:@accent-text;font-weight:600;}"
        "QPushButton#finRefresh{padding:6px 14px;}"
        "QFrame#finRow{border-radius:8px;background:transparent;}"
        "QFrame#finRow:hover{background:@bg-subtle;}"
        "QFrame#finSearchBox{background:@accent-tint;border:1px solid @border-accent;"
        "border-radius:8px;}"
        "QFrame#finSearchRow{background:transparent;border-radius:6px;}"
        "QFrame#finSearchRow:hover{background:@bg-hover;}"
        "QPushButton#finRowDel{border:none;background:transparent;color:@text-faint;"
        "font-size:12px;border-radius:10px;padding:0;}"
        "QPushButton#finRowDel:hover{background:@bg-pressed;color:@danger;}"));

    setStatusIdle();
}

QLabel *FinancePanel::makeHeaderLabel(const QString &text, int width)
{
    auto *label = new QLabel(text, this);
    label->setFixedWidth(width);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    xtools::theme::applyStyle(label, QStringLiteral(
        "font-size:12px;color:@text-faint;background:transparent;"));
    return label;
}

// ---------------------------------------------------------------- 生命周期

void FinancePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_autoRefresh->isActive())
        m_autoRefresh->start();
    refresh();   // 每次进面板都拉一次：行情过几秒就不新鲜了
}

void FinancePanel::hideEvent(QHideEvent *event)
{
    m_autoRefresh->stop();   // 看不见就别刷了，省得白打接口
    QWidget::hideEvent(event);
}

// ---------------------------------------------------------------- 分组与行

void FinancePanel::setTab(int tab)
{
    if (tab < 0 || tab >= TabCount || tab == m_tab)
        return;
    m_tab = tab;
    // 切分组时收起搜索结果：那批候选挂在「黄金」下面会让人以为它们是黄金品种
    clearSearch();
    m_addEdit->clear();
    rebuildRows();
    refresh();
}

QVector<finance::Symbol> FinancePanel::symbolsForTab() const
{
    if (m_tab == TabWatch)
        return m_watchlist;
    return presetSymbols(m_tab);
}

void FinancePanel::rebuildRows()
{
    // 清空旧行：先摘出布局（布局项要手动 delete），控件本身 deleteLater 即可。
    // 注意这会把布局末尾那颗 addStretch() 一起清掉，下面必须补回来：
    // 没有它，剩余高度会被行均分，每行被拉成两百多像素高、内容上下散架。
    while (QLayoutItem *item = m_rowsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    m_rows.clear();
    m_rowsLayout->addStretch();

    m_currentSymbols = symbolsForTab();

    if (m_currentSymbols.isEmpty()) {
        auto *empty = new QLabel(this);
        empty->setWordWrap(true);
        // 空态要说清「怎么加」，否则用户会以为工具没数据
        empty->setText(QStringLiteral(
            "自选还是空的。在上面的输入框填股票代码、基金代码或名称（如 600519、000001、黄金）"
            "回车搜索，点搜索结果即可加入；点每行末尾的 ✕ 可以移除。"));
        xtools::theme::applyStyle(empty, QStringLiteral(
            "font-size:13px;color:@text-faint;background:transparent;padding:16px 4px;"));
        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, empty);
        return;
    }

    for (const finance::Symbol &symbol : m_currentSymbols) {
        auto *row = new FinanceQuoteRow(m_rowsHost);
        row->setSymbol(symbol);

        // 有缓存先顶上，避免每次切分组都闪一下破折号
        const auto cached = m_quotes.constFind(symbol.id);
        if (cached != m_quotes.constEnd())
            row->setQuote(*cached);

        if (m_tab == TabWatch) {
            row->setDeletable(true);
            const QString id = symbol.id;
            row->setOnDelete([this, id] { removeSymbol(id); });
        }

        const QString id = symbol.id;
        row->setOnActivate([this, id] { copyQuote(id); });

        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, row);
        m_rows.insert(symbol.id, row);
    }
}

// ---------------------------------------------------------------- 行情请求

void FinancePanel::refresh()
{
    if (m_currentSymbols.isEmpty()) {
        setStatusIdle();
        return;
    }

    QStringList marketIds;
    QStringList fundCodes;
    for (const finance::Symbol &symbol : m_currentSymbols) {
        if (finance::isFundId(symbol.id))
            fundCodes << finance::fundCodeOf(symbol.id);
        else
            marketIds << symbol.id;
    }

    // 代数 ++ 之后，所有在飞的旧请求回包都会被丢弃
    const int generation = ++m_quoteGeneration;
    m_quoteError.clear();
    m_quotePending = (marketIds.isEmpty() ? 0 : 1) + (fundCodes.isEmpty() ? 0 : 1);
    if (m_quotePending == 0) {
        setStatusIdle();
        return;
    }

    setStatus(QStringLiteral("正在获取行情…"), QStringLiteral("text-muted"));

    if (!marketIds.isEmpty()) {
        sendRequest(finance::buildMarketQuoteUrl(marketIds),
                    QStringLiteral("https://quote.eastmoney.com/"), generation, false);
    }
    if (!fundCodes.isEmpty()) {
        sendRequest(finance::buildFundQuoteUrl(fundCodes),
                    QStringLiteral("https://fund.eastmoney.com/"), generation, true);
    }
}

void FinancePanel::sendRequest(const QUrl &url, const QString &referer,
                               int generation, bool isFund)
{
    QNetworkRequest request(url);
    // 8 秒超时：查询类小工具不该让用户对着「正在获取」干等
    request.setTransferTimeout(8000);
    // 两个接口都看 Referer：缺了会 403；基金接口缺 UA 时返回空数组（不报错，更难查）
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("cxk-tools/finance"));
    request.setRawHeader("Referer", referer.toUtf8());

    QNetworkReply *reply = m_nam.get(request);
    // 以 this 为上下文：面板被销毁时连接自动断开，回调不会打到已释放的对象上
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, isFund] {
        reply->deleteLater();

        // 旧代数的回包直接丢：否则「黄金」的数据会盖到「股票」上
        if (generation != m_quoteGeneration)
            return;

        if (reply->error() != QNetworkReply::NoError) {
            applyQuoteFailure(reply->errorString(), generation);
            return;
        }

        const QByteArray payload = reply->readAll();
        QVector<finance::Quote> quotes;
        QString error;
        const bool ok = isFund ? finance::parseFundQuotes(payload, &quotes, &error)
                               : finance::parseMarketQuotes(payload, &quotes, &error);
        if (!ok) {
            applyQuoteFailure(error, generation);
            return;
        }
        applyQuotes(quotes, generation);
    });
}

void FinancePanel::applyQuotes(const QVector<finance::Quote> &quotes, int generation)
{
    if (generation != m_quoteGeneration)
        return;

    for (const finance::Quote &quote : quotes) {
        m_quotes.insert(quote.id, quote);
        if (FinanceQuoteRow *row = m_rows.value(quote.id))
            row->setQuote(quote);
    }

    if (m_quotePending > 0)
        --m_quotePending;
    finishPendingRequests();
}

void FinancePanel::applyQuoteFailure(const QString &reason, int generation)
{
    if (generation != m_quoteGeneration)
        return;
    m_quoteError = reason;
    if (m_quotePending > 0)
        --m_quotePending;
    finishPendingRequests();
}

void FinancePanel::finishPendingRequests()
{
    if (m_quotePending > 0)
        return;   // 还有一批在飞，等它回来再统一写状态行

    if (!m_quoteError.isEmpty()) {
        // 失败必须写清原因：静默留白会被当成「这只票没有行情」
        setStatus(QStringLiteral("行情获取失败：%1（点「刷新」重试）").arg(m_quoteError),
                  QStringLiteral("danger-text"));
        return;
    }

    setStatus(QStringLiteral("更新于 %1 · 数据来自东方财富，仅供参考，不构成投资建议")
                  .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))),
              QStringLiteral("text-faint"));
}

// ---------------------------------------------------------------- 自选增删

void FinancePanel::startSearch()
{
    const QString keyword = m_addEdit->text().trimmed();
    if (keyword.isEmpty())
        return;

    const int generation = ++m_searchGeneration;
    m_searchPending = 2;   // 股票/指数/ETF/期货 + 场外基金，两路并发
    m_searchResults.clear();
    m_searchError.clear();
    setStatus(QStringLiteral("正在搜索「%1」…").arg(keyword), QStringLiteral("text-muted"));

    auto send = [this, generation](const QUrl &url, const QString &referer, bool funds) {
        QNetworkRequest request(url);
        request.setTransferTimeout(8000);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("cxk-tools/finance"));
        request.setRawHeader("Referer", referer.toUtf8());

        QNetworkReply *reply = m_nam.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation, funds] {
            reply->deleteLater();
            if (generation != m_searchGeneration)
                return;

            QVector<finance::Symbol> found;
            QString error;
            const QByteArray payload = reply->readAll();
            // 一路失败不影响另一路：搜索是「尽量给候选」，不是事务
            const bool ok = reply->error() == QNetworkReply::NoError
                            && (funds ? finance::parseFundSuggest(payload, &found, &error)
                                      : finance::parseSuggest(payload, &found, &error));
            if (ok) {
                // 基金搜索会顺带命中一堆持仓重合的基金，只取前几条够用了
                const int limit = funds ? 4 : 8;
                int taken = 0;
                for (const finance::Symbol &symbol : found) {
                    if (taken >= limit)
                        break;
                    bool duplicate = false;
                    for (const finance::Symbol &existing : m_searchResults) {
                        if (existing.id == symbol.id) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (duplicate)
                        continue;
                    m_searchResults.append(symbol);
                    ++taken;
                }
            } else {
                // 记下原因：两路都失败时状态行要报「搜索失败」而不是「没搜到」
                m_searchError = reply->error() != QNetworkReply::NoError
                                    ? reply->errorString()
                                    : error;
            }

            if (m_searchPending > 0)
                --m_searchPending;
            if (m_searchPending == 0)
                renderSearchResults();
        });
    };

    send(finance::buildSuggestUrl(keyword), QStringLiteral("https://www.eastmoney.com/"), false);
    send(finance::buildFundSuggestUrl(keyword), QStringLiteral("https://fund.eastmoney.com/"), true);
}

void FinancePanel::renderSearchResults()
{
    // 先清掉上一批候选（布局项要显式 delete，控件 deleteLater）
    while (QLayoutItem *item = m_searchLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    if (m_searchResults.isEmpty()) {
        m_searchBox->hide();
        if (!m_searchError.isEmpty()) {
            // 网络/接口问题必须说清楚，否则会被当成「这只票不存在」
            setStatus(QStringLiteral("搜索失败：%1（检查网络后重试）").arg(m_searchError),
                      QStringLiteral("danger-text"));
            return;
        }
        setStatus(QStringLiteral("没搜到匹配的品种，试试直接输入 6 位代码（如 600519）"),
                  QStringLiteral("warn-text"));
        return;
    }

    for (const finance::Symbol &symbol : m_searchResults) {
        auto *row = new SearchResultRow(symbol, [this, symbol] { addSymbol(symbol); },
                                        m_searchBox);
        m_searchLayout->addWidget(row);
    }

    auto *title = new QLabel(QStringLiteral("搜索结果 · 点一行加入自选"), m_searchBox);
    xtools::theme::applyStyle(title, QStringLiteral(
        "font-size:12px;color:@text-faint;background:transparent;padding:2px 4px;"));
    m_searchLayout->insertWidget(0, title);

    m_searchBox->show();
    setStatus(QStringLiteral("找到 %1 个候选品种，点击加入自选").arg(m_searchResults.size()),
              QStringLiteral("text-muted"));
}

void FinancePanel::clearSearch()
{
    // 只收起界面，不动代数：正在飞的搜索回包会被 renderSearchResults 重新弹出来，
    // 所以顺手 ++ 让它们作废
    ++m_searchGeneration;
    m_searchPending = 0;
    m_searchResults.clear();
    while (QLayoutItem *item = m_searchLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    m_searchBox->hide();
}

void FinancePanel::addSymbol(const finance::Symbol &symbol)
{
    for (const finance::Symbol &existing : m_watchlist) {
        if (existing.id == symbol.id) {
            setStatus(QStringLiteral("「%1」已经在自选里了").arg(existing.name),
                      QStringLiteral("warn-text"));
            return;
        }
    }

    m_watchlist.append(symbol);
    saveWatchlist();

    m_addEdit->clear();
    clearSearch();

    // 切到自选并重建，用户立刻能看到刚加的那一行（数据随后到）
    m_tab = TabWatch;
    if (auto *button = m_tabs->button(TabWatch))
        button->setChecked(true);
    rebuildRows();
    refresh();
    setStatus(QStringLiteral("已加入自选：%1").arg(symbol.name),
              QStringLiteral("success-text"));
}

void FinancePanel::removeSymbol(const QString &id)
{
    for (int i = 0; i < m_watchlist.size(); ++i) {
        if (m_watchlist.at(i).id != id)
            continue;
        const QString name = m_watchlist.at(i).name;
        m_watchlist.remove(i);
        saveWatchlist();
        rebuildRows();
        // 删到空之后不刷新：没有可查的品种，刷了只会报错
        if (m_currentSymbols.isEmpty())
            setStatusIdle();
        else
            refresh();
        setStatus(QStringLiteral("已从自选移除：%1").arg(name), QStringLiteral("text-muted"));
        return;
    }
}

// ---------------------------------------------------------------- 持久化

void FinancePanel::reloadWatchlist()
{
    m_watchlist.clear();

    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    // 关掉 fallback：否则未写入的键会去系统注册表里找同名值，出现「没存却有值」
    settings.setFallbacksEnabled(false);

    // beginWriteArray 会写一个 watchlist/size，用它在判断「是不是第一次运行」：
    // 用户把自选删空后这个键仍在（size=0），不会被再次塞进默认品种
    const bool exists = settings.contains(QStringLiteral("watchlist/size"));
    const int count = settings.beginReadArray(QStringLiteral("watchlist"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        finance::Symbol symbol;
        // 手改坏的单条直接跳过，不要让整块自选消失
        if (finance::decodeSymbol(settings.value(QStringLiteral("entry")).toString(), &symbol))
            m_watchlist.append(symbol);
    }
    settings.endArray();

    if (!exists) {
        m_watchlist = defaultWatchlist();
        saveWatchlist();
    }
}

void FinancePanel::saveWatchlist()
{
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    // 先整组删掉再写：只靠 beginWriteArray(size) 收缩数组时，INI 里多出来的
    // "N\entry" 不会被清掉（读的时候按 size 截断看不出问题，但文件会越积越脏）
    settings.remove(QStringLiteral("watchlist"));
    settings.beginWriteArray(QStringLiteral("watchlist"), m_watchlist.size());
    for (int i = 0; i < m_watchlist.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("entry"), finance::encodeSymbol(m_watchlist.at(i)));
    }
    settings.endArray();
    settings.sync();
}

// ---------------------------------------------------------------- 交互

void FinancePanel::copyQuote(const QString &id)
{
    const auto it = m_quotes.constFind(id);
    if (it == m_quotes.constEnd() || !it->hasPrice) {
        setStatus(QStringLiteral("这一行还没有数据，先点「刷新」试试"),
                  QStringLiteral("warn-text"));
        return;
    }

    const finance::Quote &quote = *it;
    const QString text = QStringLiteral("%1 %2 %3")
                             .arg(quote.name,
                                  finance::formatPrice(quote.price, quote.decimals),
                                  finance::formatPercent(quote.changePercent));
    QGuiApplication::clipboard()->setText(text);
    setStatus(QStringLiteral("已复制：%1").arg(text), QStringLiteral("success-text"));
}

void FinancePanel::setStatus(const QString &text, const QString &colorToken)
{
    m_status->setText(text);
    xtools::theme::applyStyle(m_status, QStringLiteral("font-size:12px;color:@%1;")
                                             .arg(colorToken));
}

void FinancePanel::setStatusIdle()
{
    if (m_currentSymbols.isEmpty()) {
        setStatus(QStringLiteral("自选为空时不会自动刷新"), QStringLiteral("text-faint"));
        return;
    }
    // 提示「能干什么」而不是空着：这一行是用户唯一会读的说明文字
    setStatus(QStringLiteral("点击任意一行复制报价；每 20 秒自动刷新，按 F5 手动刷新"),
              QStringLiteral("text-faint"));
}

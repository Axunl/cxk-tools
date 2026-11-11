#include "HotSearchPanel.h"

#include "HotSearchSource.h"        // 数据源表 + 解析（纯逻辑，可单独测）
#include "ThemePalette.h"           // 颜色令牌（宿主与插件共用，换肤靠它）

#include <QComboBox>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
// QMouseEvent 在 qwidget.h 里只是前置声明，调 event->button() 必须显式引完整定义
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QTime>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {

/// 面板最多显示多少条。B 站热门一次给 100 条，全铺出来要滚很久；50 条足够
/// 覆盖「今日热搜」的常见阅读量，也让滚动区高度稳定。
constexpr int kMaxRows = 50;

/// 单条请求的超时。热搜是「瞄一眼就走」的工具，用户不该对着「正在获取」干等。
constexpr int kTimeoutMs = 10000;

/// 全局 qss 为消掉原生 ::drop-down 甩到圆角框外的边框，写了 QComboBox 规则，
/// Qt 因此不再画下拉三角 —— 每个用 QComboBox 的面板都得自己补一个
/// （docs/PLUGIN.md 里写死的约定，参考 ProcMonPanel / TranslatePanel）。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(10, 8);
        setAttribute(Qt::WA_TransparentForMouseEvents);   // 点击穿透给下拉框
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        // 跟主题走：写死浅灰在深色底上几乎看不见
        painter.setBrush(isEnabled() ? xtools::theme::color(xtools::theme::Color::TextFaint)
                                     : xtools::theme::color(xtools::theme::Color::TextDisabled));
        QPainterPath path;
        path.moveTo(0.5, 2.0);
        path.lineTo(9.5, 2.0);
        path.lineTo(5.0, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
    }
};

/// 自动省略的 QLabel。标题往往很长（知乎热榜动不动一整句），必须按可用宽度截断
/// 加省略号；QLabel 自己只会硬裁，而 setText 里算 elidedText 时控件还没拿到最终宽度，
/// 所以把「完整文本」存下来，在 resizeEvent 里重算。
class ElidedLabel : public QLabel
{
public:
    explicit ElidedLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);       // 标题里的 < & 不该被当成富文本标签
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    void setFullText(const QString &text)
    {
        m_full = text;
        // 刻意不设自己的 tooltip：让悬停提示落到整行（HotRow 那边带简介与链接），
        // 否则鼠标停在标题上只会看到标题、移到行内其他地方提示又变了
        updateElided();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateElided();
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        // 字号变了（换肤时 applyStyle 会重设 font-size）要按新字体重新量宽度
        if (event->type() == QEvent::FontChange)
            updateElided();
    }

private:
    void updateElided()
    {
        if (m_full.isEmpty()) {
            clear();
            return;
        }
        const int available = width();
        // 宽度还没确定（构造期）时先原样放，resizeEvent 会立刻纠正
        QLabel::setText(available > 0 ? fontMetrics().elidedText(m_full, Qt::ElideRight, available)
                                      : m_full);
    }

    QString m_full;
};

/// 列表里的一行：排名 + 标题 + 标签 + 热度，整行可点。
/// 刻意不带 Q_OBJECT（无信号槽需求），点击用 std::function 回调，省掉 moc。
/// 悬停底色由父控件的 QFrame#hotRow 选择器提供，所以必须开 WA_StyledBackground。
class HotRow : public QFrame
{
public:
    HotRow(int rank, const hotsearch::Item &item, std::function<void()> onClick,
           QWidget *parent = nullptr)
        : QFrame(parent), m_onClick(std::move(onClick))
    {
        setObjectName(QStringLiteral("hotRow"));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);

        // 悬停提示带上简介与落点：标题被截断时靠它看全，鼠标停在行上也能知道会跳到哪
        QString tip = item.title;
        if (!item.desc.isEmpty())
            tip += QLatin1String("\n\n") + item.desc.left(200);
        if (!item.url.isEmpty())
            tip += QLatin1String("\n\n") + item.url;
        setToolTip(tip);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 8, 14, 8);
        layout->setSpacing(10);

        auto *rankLabel = new QLabel(QString::number(rank), this);
        rankLabel->setFixedWidth(22);
        rankLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        xtools::theme::applyStyle(
            rankLabel, QStringLiteral("font-size:13px;font-weight:%1;color:@%2;background:transparent;")
                           .arg(rank <= 3 ? QStringLiteral("600") : QStringLiteral("500"),
                                hotsearch::rankToken(rank)));
        layout->addWidget(rankLabel);

        auto *title = new ElidedLabel(this);
        title->setFullText(item.title);
        xtools::theme::applyStyle(title, QStringLiteral(
            "font-size:14px;color:@text-body;background:transparent;"));
        layout->addWidget(title, 1);

        // 标签（微博的 热 / 新 / 沸 / 荐）。其余平台没有这个字段，就不占位。
        if (!item.label.isEmpty()) {
            auto *chip = new QLabel(item.label, this);
            xtools::theme::applyStyle(
                chip, QStringLiteral("font-size:11px;font-weight:600;color:@%1;"
                                     "background:@bg-subtle;border-radius:7px;padding:1px 6px;")
                          .arg(hotsearch::labelToken(item.label)));
            layout->addWidget(chip, 0, Qt::AlignVCenter);
        }

        // 热度值右对齐定宽：各平台数值长度差很多（"8" 到 "2981万"），不定宽会让标题右边参差
        const QString hot = hotsearch::formatHot(item.hot);
        if (!hot.isEmpty()) {
            auto *hotLabel = new QLabel(hot, this);
            hotLabel->setFixedWidth(66);
            hotLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            xtools::theme::applyStyle(hotLabel, QStringLiteral(
                "font-size:12px;color:@text-faint;background:transparent;"));
            layout->addWidget(hotLabel);
        }
    }

protected:
    /// 子 QLabel 不处理鼠标事件，会冒泡到这里：整行任意位置点击都算数。
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_onClick)
            m_onClick();
        QFrame::mouseReleaseEvent(event);
    }

private:
    std::function<void()> m_onClick;
};

} // namespace

// ---------------------------------------------------------------- 面板

HotSearchPanel::HotSearchPanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    loadCurrent();   // 进面板即拉数据：热搜的价值全在「今天」两个字上
}

HotSearchPanel::~HotSearchPanel() = default;

void HotSearchPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐（同 IpQueryPanel / CalcPanel 的约定）
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(10);

    // ---------------------------------------------------------------- 工具栏
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    m_sourceBox = new QComboBox(this);
    m_sourceBox->setObjectName(QStringLiteral("hotSourceBox"));
    m_sourceBox->setMinimumWidth(132);
    m_sourceBox->setMaximumWidth(180);
    m_sourceBox->setToolTip(QStringLiteral("选择要看的榜单"));
    for (const hotsearch::Source &source : hotsearch::sources())
        m_sourceBox->addItem(source.name);
    m_sourceArrow = new ComboArrow(m_sourceBox);
    m_sourceArrow->setObjectName(QStringLiteral("hotSourceArrow"));
    m_sourceBox->installEventFilter(this);
    toolbar->addWidget(m_sourceBox);

    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setObjectName(QStringLiteral("hotRefresh"));
    m_refreshButton->setToolTip(
        QStringLiteral("重新拉取当前榜单（快捷键 %1）")
            .arg(QKeySequence(QKeySequence::Refresh).toString(QKeySequence::NativeText)));
    toolbar->addWidget(m_refreshButton);

    toolbar->addStretch(1);

    layout->addLayout(toolbar);

    // ---------------------------------------------------------------- 列表
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;border:none;}"));
    m_rowsHost = new QWidget(scroll);
    // 行样式挂在容器上，子控件按 objectName 命中：这样换肤时宿主遍历一次就能整块重算
    xtools::theme::applyStyle(m_rowsHost, QStringLiteral(
        "QFrame#hotRow{border-radius:8px;background:transparent;}"
        "QFrame#hotRow:hover{background:@bg-subtle;}"));
    m_rowsLayout = new QVBoxLayout(m_rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    // 行本身有 8px 上下内边距，行距再大就散架
    m_rowsLayout->setSpacing(2);
    scroll->setWidget(m_rowsHost);
    layout->addWidget(scroll, 1);

    // ---------------------------------------------------------------- 状态行
    // 左边状态文字（可被长文案压缩），右边是网页榜单入口：接口挂了也能自己去看。
    // 放底部而不是工具栏，是因为工具栏右侧紧挨着宿主页头的「独立窗口」按钮，
    // 两个按钮上下叠着会显得挤，而且它是兜底入口、不该占主操作位。
    auto *bottom = new QHBoxLayout;
    bottom->setSpacing(8);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("hotStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bottom->addWidget(m_status, 1);

    m_siteButton = new QPushButton(QStringLiteral("打开榜单网页"), this);
    m_siteButton->setObjectName(QStringLiteral("hotSiteButton"));
    m_siteButton->setCursor(Qt::PointingHandCursor);
    m_siteButton->setToolTip(QStringLiteral("在浏览器里打开当前榜单的网页版（接口取不到时用它兜底）"));
    xtools::theme::applyStyle(m_siteButton, QStringLiteral(
        "QPushButton{font-size:12px;color:@accent-text;background:transparent;border:none;padding:2px 0;}"
        "QPushButton:hover{color:@accent;}"));
    bottom->addWidget(m_siteButton);

    layout->addLayout(bottom);

    setStatus(QStringLiteral("点击条目在浏览器中打开"), false);

    // ---------------------------------------------------------------- 交互
    connect(m_sourceBox, &QComboBox::currentIndexChanged, this, [this] { loadCurrent(); });
    connect(m_refreshButton, &QPushButton::clicked, this, &HotSearchPanel::loadCurrent);
    connect(m_siteButton, &QPushButton::clicked, this, [this] {
        const int index = m_sourceBox->currentIndex();
        const QVector<hotsearch::Source> &list = hotsearch::sources();
        if (index >= 0 && index < list.size())
            openInBrowser(list.at(index).siteUrl);
    });

    // F5 刷新：WidgetWithChildrenShortcut 保证只在本面板生效，
    // 不跟主窗口 / 独立窗口的其他快捷键抢按键
    auto *refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
    refreshShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(refreshShortcut, &QShortcut::activated, this, &HotSearchPanel::loadCurrent);
}

bool HotSearchPanel::eventFilter(QObject *watched, QEvent *event)
{
    // 箭头要跟着下拉框的尺寸走：宽度随面板变（独立窗口 / 主窗口两种宽度），
    // 只在构造时摆一次会在 Resize 后跑到框外
    if (watched == m_sourceBox && m_sourceArrow) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            const QRect box = m_sourceBox->rect();
            m_sourceArrow->move(box.right() - 18,
                                box.top() + (box.height() - m_sourceArrow->height()) / 2);
            m_sourceArrow->raise();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void HotSearchPanel::loadCurrent()
{
    const QVector<hotsearch::Source> &list = hotsearch::sources();
    const int index = m_sourceBox->currentIndex();
    if (index < 0 || index >= list.size())
        return;

    // 代数自增：之前在飞的请求回包时代数对不上，会被丢弃（见头文件里的说明）
    const int generation = ++m_generation;

    m_refreshButton->setEnabled(false);
    clearRows();
    showPlaceholder(QStringLiteral("正在获取「%1」…").arg(list.at(index).name), false);
    setStatus(QStringLiteral("正在获取…"), false);

    requestAttempt(index, 0, generation);
}

void HotSearchPanel::requestAttempt(int sourceIndex, int attempt, int generation)
{
    const QVector<hotsearch::Source> &list = hotsearch::sources();
    if (sourceIndex < 0 || sourceIndex >= list.size())
        return;

    const hotsearch::Source source = list.at(sourceIndex);
    const QString url = attempt == 0 ? source.apiUrl : source.fallbackUrl;
    if (url.isEmpty()) {
        applyFailure(source, QStringLiteral("这个榜单没有可用的数据接口"));
        return;
    }

    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(kTimeoutMs);
    // 统一用常见浏览器 UA：百度官方接口会拒掉非浏览器 UA（直接 403），
    // 聚合实例也普遍校验 UA，用 "cxk-tools/1.0" 这种标识会被当成爬虫
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                                     "Chrome/120.0.0.0 Safari/537.36"));
    request.setRawHeader("Accept", "application/json, text/plain, */*");
    // 百度按 Referer 判来源，缺了偶尔会给空 cards
    request.setRawHeader("Referer", source.siteUrl.toUtf8());

    QNetworkReply *reply = m_nam.get(request);
    // 以 this 为上下文：面板销毁（离开工具页）时连接自动断开，不会回调到已释放对象
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, sourceIndex, attempt, generation, url, source] {
                reply->deleteLater();

                // 用户已经切到别的榜单 / 又点了一次刷新 → 这份回包作废，
                // 否则「微博」的数据会盖到「知乎」上
                if (generation != m_generation)
                    return;

                QString reason;
                hotsearch::Result result;
                if (reply->error() != QNetworkReply::NoError) {
                    // errorString() 只说「服务器拒绝访问」，带上 HTTP 码才看得出是 403 还是 5xx
                    const int status = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    reason = status > 0
                                 ? QStringLiteral("%1（HTTP %2）").arg(reply->errorString()).arg(status)
                                 : reply->errorString();
                } else {
                    result = hotsearch::parseResponse(url, reply->readAll());
                    reason = result.error;
                }

                if (reason.isEmpty()) {
                    applyResult(source, result, attempt);
                    return;
                }

                // 主接口失败且配了备胎（目前只有百度）：自动换一次，不静默
                if (attempt == 0 && !source.fallbackUrl.isEmpty()) {
                    setStatus(QStringLiteral("主接口取不到（%1），正在换备用源…").arg(reason), false);
                    requestAttempt(sourceIndex, 1, generation);
                    return;
                }
                applyFailure(source, reason);
            });
}

void HotSearchPanel::applyResult(const hotsearch::Source &source, const hotsearch::Result &result,
                                 int attempt)
{
    clearRows();

    int shown = 0;
    for (const hotsearch::Item &item : result.items) {
        if (shown >= kMaxRows)
            break;
        addRow(++shown, item);
    }
    m_rowsLayout->addStretch(1);   // 条目贴顶排列，不留空档

    m_refreshButton->setEnabled(true);

    QString status = QStringLiteral("「%1」%2 条 · %3 更新")
                         .arg(source.name)
                         .arg(shown)
                         .arg(QTime::currentTime().toString(QStringLiteral("HH:mm")));
    if (attempt > 0)
        status += QStringLiteral(" · 备用源");
    setStatus(status, false);
}

void HotSearchPanel::applyFailure(const hotsearch::Source &source, const QString &reason)
{
    m_refreshButton->setEnabled(true);
    clearRows();
    // 不在状态行堆错误详情（会被截断），整段放列表区，并明确给出「自己去看」的出口
    showPlaceholder(QStringLiteral("获取「%1」失败\n%2\n\n点右下角的「打开榜单网页」可直接看榜单，"
                                   "或稍后点「刷新」重试。")
                        .arg(source.name, reason),
                    true);
    setStatus(QStringLiteral("「%1」获取失败").arg(source.name), true);
}

void HotSearchPanel::clearRows()
{
    // 先隐藏并脱离父控件再 deleteLater：只 deleteLater 的话，旧行会一直画到事件循环
    // 处理完删除事件为止，视觉上就是新旧榜单叠在一起
    while (QLayoutItem *item = m_rowsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

void HotSearchPanel::addRow(int rank, const hotsearch::Item &item)
{
    auto *row = new HotRow(rank, item, [this, url = item.url] { openInBrowser(url); }, m_rowsHost);
    m_rowsLayout->addWidget(row);
}

void HotSearchPanel::showPlaceholder(const QString &text, bool isError)
{
    auto *label = new QLabel(text, m_rowsHost);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    xtools::theme::applyStyle(
        label, QStringLiteral("font-size:12px;padding:16px 12px;color:@%1;")
                   .arg(isError ? QStringLiteral("danger-text") : QStringLiteral("text-faint")));
    m_rowsLayout->addWidget(label);
    m_rowsLayout->addStretch(1);
}

void HotSearchPanel::setStatus(const QString &text, bool isError)
{
    // 错误用 danger-text（深色主题下是浅红），提示用 text-faint；都不写死色值
    m_status->setText(text);
    xtools::theme::applyStyle(
        m_status, QStringLiteral("font-size:12px;color:@%1;")
                      .arg(isError ? QStringLiteral("danger-text") : QStringLiteral("text-faint")));
}

void HotSearchPanel::openInBrowser(const QString &url)
{
    const QUrl target(url);
    if (url.isEmpty() || !target.isValid() || target.scheme().isEmpty()) {
        setStatus(QStringLiteral("这条没有可打开的链接"), true);
        return;
    }
    if (!QDesktopServices::openUrl(target))
        setStatus(QStringLiteral("打不开浏览器（系统没有设置默认浏览器？）"), true);
}

#include "HomePage.h"

#include "IconBadge.h"
#include "LayoutMetrics.h"
#include "LocalAppIndex.h"
#include "PluginManager.h"
#include "QuickCalc.h"
#include "Settings.h"
#include "ThemePalette.h"
#include "ToolRegistry.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// 首页布局常量。左右留白统一走 xtools::layout::kPageMargin（与 MainWindow 页头一致，
// 换页时标题不会跳），其余磁贴尺寸是首页私有的，留在这里。
constexpr int kTileWidth = 104;
constexpr int kTileHeight = 100;
constexpr int kTileIcon = 48;
/// 磁贴圆角。自绘选中/悬停底色时用（见 LauncherTile::paintEvent），
/// 必须与 qss 里 `#launcherTile` 的 border-radius 一致，否则圆角会「两套」。
constexpr int kTileRadius = 10;
constexpr int kTileIconTop = 12;      ///< 图标上方的呼吸空间
constexpr int kTileSpacing = 6;       ///< 图标与名称之间
constexpr int kGridSpacing = 8;       ///< 磁贴彼此间距
/// 算列数时恒定扣掉的宽度：竖滚动条的出现/消失会让 viewport 宽度跳变，
/// 若按真实宽度算列数，会出现「少一列→内容变矮→滚动条消失→多一列→变高→滚动条出现」
/// 的无限抖动。恒定预留后列数对滚动条不敏感，代价是最右侧最多浪费 12px。
constexpr int kScrollbarReserve = 12;
constexpr int kSearchHeight = 40;
constexpr int kSearchClearSize = 20;
constexpr int kSearchClearGap = 8;
/// 搜索框内结果徽标（「= 96」）的尺寸规则。
/// 宽度在 C++ 侧按像素算（不用 qss 的 padding）：徽标是 QLineEdit 的子控件，
/// 几何完全手动控制，qss 的 padding 只会让文字被裁而不是把控件撑大。
constexpr int kInlineResultMaxWidth = 200;   ///< 上限，2^1000 这类结果按像素省略
constexpr int kInlineResultPad = 8;          ///< 徽标左右内边距（C++ 侧算进宽度）
constexpr int kInlineGap = 6;                ///< 徽标与 ✕ / 文字之间的间距
constexpr int kInlineFeedbackMs = 1200;      ///< 「已复制」停留时长
constexpr int kSearchTextPad = 12;           ///< 与 qss 里搜索框的左 padding 对齐
constexpr int kNameMaxWidth = kTileWidth - 12;
constexpr int kIntentIcon = 34;   ///< 建议条左侧工具图标
/// 数字角标：18px 方块放在左上角的 3px 留白里。位置写死是因为磁贴尺寸固定、
/// 图标居中（x 从 28 开始），角标只占 x∈[3,21]，不会碰到图标。
constexpr int kShortcutSize = 18;
constexpr int kShortcutMargin = 3;
/// 可直选的项数上限：键盘上只有 1~9 九个直选位，第 10 项起只能方向键/回车。
constexpr int kMaxShortcut = 9;

} // namespace

// ---------------------------------------------------------------------------
// LauncherTile
// ---------------------------------------------------------------------------

LauncherTile::LauncherTile(const LauncherItem &item, QWidget *parent)
    : QWidget(parent)
    , m_item(item)
    , m_fullName(item.name)
    , m_icon(item.icon)
{
    setObjectName(QStringLiteral("launcherTile"));
    // 不用 WA_StyledBackground：选中/悬停的底色在 paintEvent 里自绘（原因见那里），
    // qss 只负责圆角尺寸这类静态约定，运行期不再依赖样式重算。
    setFixedSize(kTileWidth, kTileHeight);
    setCursor(Qt::PointingHandCursor);

    // 应用的 description 是完整路径，tooltip 里带上名字更像「这是哪个文件」
    if (m_item.kind == LauncherItem::Kind::App)
        setToolTip(m_item.name + QLatin1Char('\n') + m_item.description);
    else
        setToolTip(m_item.description);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, kTileIconTop, 0, 8);
    layout->setSpacing(kTileSpacing);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setObjectName(QStringLiteral("tileIcon"));
    m_iconLabel->setFixedSize(kTileIcon, kTileIcon);
    m_iconLabel->setAlignment(Qt::AlignCenter);

    m_name = new QLabel(m_fullName, this);
    m_name->setObjectName(QStringLiteral("tileName"));
    m_name->setAlignment(Qt::AlignCenter);
    m_name->setWordWrap(false);

    auto *iconRow = new QHBoxLayout;
    iconRow->setContentsMargins(0, 0, 0, 0);
    iconRow->addStretch(1);
    iconRow->addWidget(m_iconLabel);
    iconRow->addStretch(1);

    layout->addLayout(iconRow);
    layout->addWidget(m_name);

    // 数字角标：绝对定位而不是塞进上面那个 QVBoxLayout —— 布局会把它当成第三行，
    // 磁贴固定 100 高，多一行就会把图标和名称往上挤。默认隐藏，由 setShortcutNumber 控制。
    m_shortcut = new QLabel(this);
    m_shortcut->setObjectName(QStringLiteral("tileShortcut"));
    m_shortcut->setAlignment(Qt::AlignCenter);
    m_shortcut->setFixedSize(kShortcutSize, kShortcutSize);
    m_shortcut->move(kShortcutMargin, kShortcutMargin);
    m_shortcut->hide();
}

void LauncherTile::setShortcutNumber(int number)
{
    m_shortcutNumber = number > 0 ? number : 0;

    if (m_shortcutNumber <= 0) {
        m_shortcut->hide();
        return;
    }

    m_shortcut->setText(QString::number(m_shortcutNumber));
    m_shortcut->show();
    // 图标是后加进布局的兄弟控件，raise 一下保证角标压在图标/底色之上
    m_shortcut->raise();
}

void LauncherTile::setCurrent(bool on)
{
    if (m_current == on)
        return;
    m_current = on;
    // 只重画，不 unpolish/polish：改动态属性后 Qt 要为这个磁贴**和它的子控件**
    // 重算样式，还会把几何/尺寸提示标脏，方向键连按时每一步都要重排一次，
    // 观感就是「按了跟不上」。底色改由 paintEvent 自己画，见那里的说明。
    update();

    // 角标跟着提亮：qss 的祖先选择器（`#launcherTile[current=true] #tileShortcut`）
    // 依赖动态属性刷新，而那正是上面要省掉的动作，所以这里直接给小角标换一套
    // 内联样式。颜色仍然走令牌，换肤时 Theme 会重新套用这串模板。
    if (m_shortcutNumber > 0) {
        xtools::theme::applyStyle(m_shortcut,
                                  on ? QStringLiteral("color: @accent-text; border-color: @border-accent;")
                                     : QStringLiteral("color: @text-muted; border-color: @border;"));
    }
}

void LauncherTile::setHovered(bool on)
{
    if (m_hovered == on)
        return;
    m_hovered = on;
    update();   // 同上：悬停也只是重画，不做样式重算
}

void LauncherTile::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    // 没有状态就什么都不画：磁贴本身透明，露出首页底色。
    if (!m_current && !m_hovered)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 选中态比 hover 深一档：hover 是「鼠标恰好路过」，选中是「回车就打开它」，
    // 两者必须能一眼分开。色值全部走主题令牌，换肤时 Theme 会挨个 update()。
    const QColor fill = xtools::theme::color(m_current ? xtools::theme::Color::AccentSoftStrong
                                                      : xtools::theme::Color::AccentSoft);
    // 内缩 0.5px：1px 的描边压在像素中心才不会糊成 2px 的灰边
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    painter.setBrush(fill);
    if (m_current) {
        painter.setPen(QPen(xtools::theme::color(xtools::theme::Color::BorderAccentStrong), 1.0));
    } else {
        painter.setPen(Qt::NoPen);
    }
    painter.drawRoundedRect(box, kTileRadius, kTileRadius);
}

void LauncherTile::updateNameText()
{
    if (!m_name)
        return;
    // 名称超长就省略：磁贴固定宽度，QLabel 不会自己截断，会直接溢出到邻居上
    const QFontMetrics fm(m_name->font());
    m_name->setText(fm.elidedText(m_fullName, Qt::ElideRight, kNameMaxWidth));
    m_name->setToolTip(m_fullName);
}

void LauncherTile::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    // 图标按真实 DPR 取：构造时窗口还没落到屏幕上，devicePixelRatioF() 不可靠
    QPixmap pm = m_icon.pixmap(QSize(kTileIcon, kTileIcon), devicePixelRatioF());
    if (pm.isNull())   // 插件图标文件坏了、或系统取不到应用图标时兜底
        pm = IconBadge::pixmap(m_item.id, m_fullName, kTileIcon, devicePixelRatioF());
    m_iconLabel->setPixmap(pm);

    // 字体（qss 里的 12px）此时才生效，省略宽度要按最终字体算
    updateNameText();
}

void LauncherTile::mouseReleaseEvent(QMouseEvent *event)
{
    // 只认「在磁贴内按下并抬起」的左键；右键交给 contextMenuEvent
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        emit activated(m_item.id);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void LauncherTile::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);

    if (m_item.kind == LauncherItem::Kind::App) {
        QAction *open = menu.addAction(QStringLiteral("打开"));
        QAction *reveal = menu.addAction(QStringLiteral("在文件管理器中显示"));
        QAction *chosen = menu.exec(event->globalPos());
        if (chosen == open)
            emit activated(m_item.id);
        else if (chosen == reveal)
            emit revealRequested(m_item.id);
    } else {
        QAction *popOut = menu.addAction(QStringLiteral("在独立窗口打开"));
        if (menu.exec(event->globalPos()) == popOut)
            emit popOutRequested(m_item.id);
    }
    event->accept();
}

void LauncherTile::enterEvent(QEnterEvent *event)
{
    setHovered(true);
    QWidget::enterEvent(event);
}

void LauncherTile::leaveEvent(QEvent *event)
{
    setHovered(false);
    QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
// IntentBanner
// ---------------------------------------------------------------------------

IntentBanner::IntentBanner(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("intentBanner"));
    // 与 LauncherTile 同一原因：QWidget 子类的 qss 背景必须靠 WA_StyledBackground 才画
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(12);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setObjectName(QStringLiteral("intentIcon"));
    m_iconLabel->setFixedSize(kIntentIcon, kIntentIcon);
    m_iconLabel->setAlignment(Qt::AlignCenter);

    auto *textBox = new QVBoxLayout;
    textBox->setContentsMargins(0, 0, 0, 0);
    textBox->setSpacing(2);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("intentTitle"));
    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("intentDetail"));
    // 详情是内容摘要，可能很长：锁死单行，并且不参与「撑宽布局」——
    // 否则一段长 JSON 会把建议条的最小宽度顶到窗口之外（首页横向滚动条是关掉的）。
    m_detail->setWordWrap(false);
    m_detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_detail->setTextInteractionFlags(Qt::NoTextInteraction);

    textBox->addWidget(m_title);
    textBox->addWidget(m_detail);

    // 右侧「回车」提示：告诉用户不必再点，按回车就是这个建议
    auto *hint = new QLabel(QStringLiteral("Enter 打开"), this);
    hint->setObjectName(QStringLiteral("intentHint"));

    layout->addWidget(m_iconLabel);
    layout->addLayout(textBox, 1);
    layout->addWidget(hint, 0, Qt::AlignVCenter);
}

void IntentBanner::setContent(const QIcon &icon, const QString &title, const QString &detail)
{
    m_icon = icon;
    m_title->setText(title);
    m_detailText = detail;
    m_detail->setText(detail);   // 先给原文，宽度确定后再按需省略

    QPixmap pm = m_icon.pixmap(QSize(kIntentIcon, kIntentIcon), devicePixelRatioF());
    if (pm.isNull())
        pm = IconBadge::pixmap(title, title.left(1), kIntentIcon, devicePixelRatioF());
    m_iconLabel->setPixmap(pm);

    updateElidedDetail();
}

void IntentBanner::updateElidedDetail()
{
    if (m_detailText.isEmpty() || !m_detail)
        return;
    const int available = m_detail->width();
    if (available <= 0)
        return;
    const QFontMetrics fm(m_detail->font());
    m_detail->setText(fm.elidedText(m_detailText, Qt::ElideRight, available));
}

void IntentBanner::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 布局是在 resizeEvent **之后**才更新子控件几何的，所以这里读到的
    // m_detail->width() 还是旧值。延后一拍（事件循环回到空闲）再按真实宽度省略。
    QTimer::singleShot(0, this, [this] { updateElidedDetail(); });
}

void IntentBanner::setHovered(bool on)
{
    if (m_hovered == on)
        return;
    m_hovered = on;
    setProperty("hovered", on);
    // 动态属性变了必须 unpolish/polish，qss 选择器才会重新求值（与 LauncherTile 一致）
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void IntentBanner::mouseReleaseEvent(QMouseEvent *event)
{
    // 只认「在建议条内按下并抬起」的左键
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        emit activated();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void IntentBanner::enterEvent(QEnterEvent *event)
{
    setHovered(true);
    QWidget::enterEvent(event);
}

void IntentBanner::leaveEvent(QEvent *event)
{
    setHovered(false);
    QWidget::leaveEvent(event);
}

void IntentBanner::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 构造时窗口还没落到屏幕上，devicePixelRatioF() 不可靠，图标要在这里重取一次
    if (!m_icon.isNull()) {
        const QPixmap pm = m_icon.pixmap(QSize(kIntentIcon, kIntentIcon), devicePixelRatioF());
        if (!pm.isNull())
            m_iconLabel->setPixmap(pm);
    }
    // 字体（qss 里的字号）到显示时才最终生效，省略宽度要按最终字体重算
    QTimer::singleShot(0, this, [this] { updateElidedDetail(); });
}

// ---------------------------------------------------------------------------
// HomePage
// ---------------------------------------------------------------------------

HomePage::HomePage(ToolRegistry *registry, Settings *settings, PluginManager *plugins,
                   LocalAppIndex *apps, QWidget *parent)
    : QWidget(parent)
    , m_registry(registry)
    , m_settings(settings)
    , m_plugins(plugins)
    , m_apps(apps)
{
    setObjectName(QStringLiteral("homeRoot"));
    buildUi();
    rebuild();
}

void HomePage::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---------------------------------------------------------------- 搜索行
    m_top = new QWidget(this);
    m_top->setObjectName(QStringLiteral("homeTop"));
    auto *topLayout = new QHBoxLayout(m_top);
    topLayout->setContentsMargins(xtools::layout::kPageMargin, 18, xtools::layout::kPageMargin, 6);
    topLayout->setSpacing(0);

    m_search = new QLineEdit(m_top);
    m_search->setObjectName(QStringLiteral("homeSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索工具或本机应用，回车打开…"));
    m_search->setFixedHeight(kSearchHeight);
    m_search->installEventFilter(this);   // 回车/方向键/Esc + 清空按钮摆位
    topLayout->addWidget(m_search);

    // 自建清空按钮而不是用 QLineEdit 自带的：自带按钮贴着控件右缘，
    // 与圆角边框的视觉间距不可控（旧版踩过），自建可以精确摆位。
    m_searchClear = new QToolButton(m_search);
    m_searchClear->setObjectName(QStringLiteral("homeSearchClear"));
    m_searchClear->setText(QStringLiteral("\u2715"));   // ✕，与标题栏关闭同字形
    m_searchClear->setFixedSize(kSearchClearSize, kSearchClearSize);
    m_searchClear->setCursor(Qt::PointingHandCursor);
    m_searchClear->setFocusPolicy(Qt::NoFocus);   // 不抢焦点，点完还能继续输入
    m_searchClear->hide();
    // 点 ✕ 要连粘贴进来的图片一起清掉，所以不能直接接 QLineEdit::clear
    connect(m_searchClear, &QToolButton::clicked, this, &HomePage::clearSearch);

    // 算式结果徽标：贴在框内右侧，与 ✕ 同一层（都是 QLineEdit 的子控件、手动摆位）。
    // 为什么塞进输入框而不是做成结果条：用户要的是「就地看到答案」——视线不用离开
    // 正在敲的那行字，也不占网格的空间，更不会替用户打开一个他不需要的面板。
    m_inlineResult = new QLabel(m_search);
    m_inlineResult->setObjectName(QStringLiteral("homeSearchResult"));
    m_inlineResult->setAlignment(Qt::AlignCenter);
    m_inlineResult->setFocusPolicy(Qt::NoFocus);   // 不抢焦点，点完仍能继续输入
    m_inlineResult->setCursor(Qt::PointingHandCursor);
    m_inlineResult->installEventFilter(this);      // 点击 = 复制
    m_inlineResult->hide();

    m_inlineTimer = new QTimer(this);
    m_inlineTimer->setSingleShot(true);
    m_inlineTimer->setInterval(kInlineFeedbackMs);
    connect(m_inlineTimer, &QTimer::timeout, this, [this] { setInlineFeedback(false); });

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_searchClear->setVisible(!text.isEmpty());
        // 粘贴图片后用户又开始打字：以文字为准，把图片丢掉（否则会一直停在 OCR 建议上）
        if (!text.isEmpty() && !m_pendingImage.isNull())
            clearPendingImage();
        rebuild();
    });

    // ---------------------------------------------------------------- 网格
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("homeScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 列数按视口宽度算，所以视口自己也要触发重排：首页被 setCurrentWidget 显示时
    // 视口的真实宽度是在布局之后才确定的，只靠 HomePage::resizeEvent 会拿到旧宽度，
    // 表现为「首帧只排一列，之后再也不重排」。
    m_scroll->viewport()->installEventFilter(this);

    m_content = new QWidget;
    m_content->setObjectName(QStringLiteral("homeContent"));
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(xtools::layout::kPageMargin, 4, xtools::layout::kPageMargin, 16);
    contentLayout->setSpacing(2);
    m_scroll->setWidget(m_content);

    // 每组 = 标题 + 一行「网格 + 尾部 stretch」。
    // 尾部 stretch 不能省：QGridLayout 遇到多余宽度会拉伸列，磁贴间距就会忽大忽小。
    auto makeGroup = [this](const QString &title, QWidget **box, QLabel **titleLabel, QGridLayout **grid) {
        auto *group = new QWidget(m_content);
        auto *groupLayout = new QVBoxLayout(group);
        groupLayout->setContentsMargins(0, 0, 0, 0);
        groupLayout->setSpacing(6);

        auto *label = new QLabel(title, group);
        label->setObjectName(QStringLiteral("sectionTitle"));

        auto *gridLayout = new QGridLayout;
        gridLayout->setContentsMargins(0, 0, 0, 0);
        gridLayout->setSpacing(kGridSpacing);

        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addLayout(gridLayout);
        row->addStretch(1);

        groupLayout->addWidget(label);
        groupLayout->addLayout(row);

        *box = group;
        *titleLabel = label;
        *grid = gridLayout;
    };
    makeGroup(QStringLiteral("最近使用"), &m_recentBox, &m_recentTitle, &m_recentGrid);
    makeGroup(QStringLiteral("全部"), &m_allBox, &m_allTitle, &m_allGrid);
    makeGroup(QStringLiteral("本机应用"), &m_appBox, &m_appTitle, &m_appGrid);

    // 建议条放在所有分组之上：它是「这段输入该交给谁」的直接答案，必须在视线第一落点
    m_intent = new IntentBanner(m_content);
    m_intent->hide();
    connect(m_intent, &IntentBanner::activated, this, &HomePage::activateIntent);

    // 搜索无命中：与「一个工具都没有」是两回事，文案与处理都分开
    m_noResult = new QLabel(QStringLiteral("没有找到匹配的工具或应用"), m_content);
    m_noResult->setObjectName(QStringLiteral("homeHint"));
    m_noResult->setAlignment(Qt::AlignCenter);

    // 空工具引导（原主窗口的空状态页，搬到首页后不再需要独立页面）
    m_emptyBox = new QWidget(m_content);
    auto *emptyLayout = new QVBoxLayout(m_emptyBox);
    emptyLayout->setContentsMargins(0, 40, 0, 0);
    emptyLayout->setSpacing(12);

    auto *emptyTitle = new QLabel(QStringLiteral("还没有加载到任何工具"), m_emptyBox);
    emptyTitle->setObjectName(QStringLiteral("homeEmptyTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);

    auto *emptyDesc = new QLabel(
        QStringLiteral("把编译好的插件动态库放进 plugins 目录，然后重启应用。\n"
                       "想自己写一个工具，看 docs/PLUGIN.md（不到 30 行就能上手）。"),
        m_emptyBox);
    emptyDesc->setObjectName(QStringLiteral("homeEmptyDesc"));
    emptyDesc->setAlignment(Qt::AlignCenter);

    auto *openDirBtn = new QPushButton(QStringLiteral("打开插件目录"), m_emptyBox);
    openDirBtn->setProperty("accent", true);
    openDirBtn->setFixedWidth(140);
    connect(openDirBtn, &QPushButton::clicked, this, [this] {
        const QString dir = m_plugins ? m_plugins->pluginDir() : QString();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(openDirBtn);
    btnRow->addStretch(1);

    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyDesc);
    emptyLayout->addLayout(btnRow);

    contentLayout->addWidget(m_intent);
    contentLayout->addWidget(m_recentBox);
    contentLayout->addWidget(m_allBox);
    contentLayout->addWidget(m_appBox);
    contentLayout->addWidget(m_noResult);
    contentLayout->addWidget(m_emptyBox);
    contentLayout->addStretch(1);

    m_footer = new QLabel(this);
    m_footer->setObjectName(QStringLiteral("homeFooter"));

    // 数字键提示单独一块：与左边「已加载 N 个工具」不是同一类信息，
    // 拼进同一个字符串会被读成第三项统计。没有角标时整块隐藏。
    m_shortcutHint = new QLabel(this);
    m_shortcutHint->setObjectName(QStringLiteral("homeShortcutHint"));
    m_shortcutHint->hide();

    // 页脚左右留白挪到这层布局上（qss 的 #homeFooter 不再带 padding），
    // 这样两块文字共用同一条基线与左右边界。
    auto *footerRow = new QHBoxLayout;
    footerRow->setContentsMargins(xtools::layout::kPageMargin, 4, xtools::layout::kPageMargin, 10);
    footerRow->setSpacing(12);
    footerRow->addWidget(m_footer);
    footerRow->addStretch(1);
    footerRow->addWidget(m_shortcutHint);

    layout->addWidget(m_top);
    layout->addWidget(m_scroll, 1);
    layout->addLayout(footerRow);
}

void HomePage::clearTiles()
{
    // 立即删除而不是 deleteLater：紧接着就要重建并 addWidget，
    // 延迟删除会让旧 QWidgetItem 短暂留在布局里，重排结果不可预期。
    for (LauncherTile *tile : m_tiles)
        delete tile;
    m_tiles.clear();
    m_recentTiles.clear();
    m_allTiles.clear();
    m_appTiles.clear();
    m_currentIndex = -1;
    m_currentByUser = false;
}

void HomePage::rebuild()
{
    clearTiles();

    const QString query = m_search->text().trimmed();
    const int toolCount = m_registry ? m_registry->count() : 0;
    const int appCount = m_apps ? m_apps->count() : 0;

    // 工具和应用都没有才显示引导页；只有应用没有工具时搜索仍然可用
    if (toolCount == 0 && appCount == 0) {
        showEmptyState(true);
        updateFooter();
        return;
    }
    showEmptyState(false);

    auto addTool = [this](const ToolEntry &entry, QList<LauncherTile *> *bucket) {
        LauncherItem item;
        item.kind = LauncherItem::Kind::Tool;
        item.id = entry.id();
        item.name = entry.name();
        item.description = entry.info.description;
        item.icon = entry.icon;

        auto *tile = new LauncherTile(item, m_content);
        connect(tile, &LauncherTile::activated, this, &HomePage::toolActivated);
        connect(tile, &LauncherTile::popOutRequested, this, &HomePage::toolPopOutRequested);
        connect(tile, &LauncherTile::activated, this, [this, tile] { selectTileFromMouse(tile); });
        bucket->append(tile);
    };

    auto addApp = [this](const LocalAppIndex::Entry &entry, QList<LauncherTile *> *bucket) {
        LauncherItem item;
        item.kind = LauncherItem::Kind::App;
        item.id = entry.id;
        item.name = entry.name;
        item.description = entry.path;
        item.icon = m_apps ? m_apps->icon(entry.id) : QIcon();

        auto *tile = new LauncherTile(item, m_content);
        connect(tile, &LauncherTile::activated, this, &HomePage::appActivated);
        connect(tile, &LauncherTile::revealRequested, this, [this](const QString &id) {
            if (m_apps)
                m_apps->reveal(id);
        });
        connect(tile, &LauncherTile::activated, this, [this, tile] { selectTileFromMouse(tile); });
        bucket->append(tile);
    };

    if (!m_pendingImage.isNull()) {
        // 贴图时首页只回答一件事：要不要识别这张图。不列任何磁贴，避免抢视线。
        // （关键词在 setPendingImage 里已被清空，这里若继续走 query.isEmpty() 分支
        //   会把整个「全部」网格铺出来，建议条反而被挤下去。）
    } else if (query.isEmpty()) {
        m_allTitle->setText(QStringLiteral("全部"));

        // 最近使用：工具和本机应用共用一个时间序列。键里可能残留已卸载的插件/
        // 已删除的应用，两边都解析不到就跳过（不显示一个点不开的磁贴）。
        if (m_settings) {
            for (const QString &id : m_settings->recentToolIds()) {
                if (const ToolEntry *entry = m_registry ? m_registry->entryById(id) : nullptr)
                    addTool(*entry, &m_recentTiles);
                else if (const LocalAppIndex::Entry *app = m_apps ? m_apps->entryById(id) : nullptr)
                    addApp(*app, &m_recentTiles);
            }
        }

        // 「全部」用加载顺序而不是使用频次：网格比列表更依赖位置记忆，
        // 按频次排会让磁贴随使用漂移，找东西全靠扫。
        if (m_registry) {
            for (const ToolEntry &entry : m_registry->entries())
                addTool(entry, &m_allTiles);
        }
    } else {
        // 搜索走相关度排序，第一个就是回车要打开的那个
        if (m_registry) {
            const QStringList hits = m_registry->search(query, 1000);
            for (const QString &id : hits) {
                if (const ToolEntry *entry = m_registry->entryById(id))
                    addTool(*entry, &m_allTiles);
            }
        }

        if (m_apps) {
            // 应用图标是系统查询，取太多会在输入过程中卡顿；12 个足够一屏浏览
            const QStringList appHits = m_apps->search(query, 12);
            for (const QString &id : appHits) {
                if (const LocalAppIndex::Entry *entry = m_apps->entryById(id))
                    addApp(*entry, &m_appTiles);
            }
        }

        // 只有一类命中时标题不带区分度；两类都有时分别标「工具」「本机应用」
        m_allTitle->setText(m_appTiles.isEmpty() ? QStringLiteral("搜索结果")
                                                 : QStringLiteral("工具"));
    }

    // 建议条：贴图时它独占首页（此时上面两个分支不会产出任何磁贴），
    // 文字输入时要先知道「有没有命中」才能决定要不要给翻译建议。
    // 就地求值要排在建议之前：它决定了这段输入是不是「已经算出了答案」，
    // 而这会改变建议的判定（见下面 updateIntent 的第二个实参）。
    updateInlineResult();

    const bool hasHits = !m_allTiles.isEmpty() || !m_appTiles.isEmpty();
    // 已经就地算出结果时不再给翻译建议：`pi*2` 这类输入会被 isEnglishText 判成英文，
    // 结果左边挂一张「翻译 pi*2」的建议条，右边却又显示 6.28，自相矛盾。
    updateIntent(query, hasHits || !m_inlineValue.isEmpty());

    // 分组显隐统一按内容判断：「没有工具只有应用」时不能留下一个空的「全部」标题
    m_recentBox->setVisible(!m_recentTiles.isEmpty());
    m_allBox->setVisible(!m_allTiles.isEmpty());
    m_appBox->setVisible(!m_appTiles.isEmpty());

    // 「什么都没搜到」分两种：给了建议就把位置让给建议条，没建议才提示找不到。
    // 贴图但 OCR 插件没加载也走这里，文案要具体（否则用户会以为图片没粘上）。
    // 已经就地算出算式结果时同样不提示：答案就在搜索框里，再喊一句「没找到」
    // 只会让用户以为结果不作数。
    const bool imageMode = !m_pendingImage.isNull();
    m_noResult->setText(imageMode
                            ? QStringLiteral("已粘贴图片，但没找到能识别文字的 OCR 工具")
                            : QStringLiteral("没有找到匹配的工具或应用"));
    m_noResult->setVisible((imageMode || !query.isEmpty()) && !hasHits && !m_intentData.valid()
                           && m_inlineValue.isEmpty());

    // 视觉顺序 = 键盘导航顺序 = 数字角标顺序
    m_tiles = m_recentTiles + m_allTiles + m_appTiles;
    applyShortcutNumbers();

    m_lastColumns = -1;   // 磁贴全换了，强制重新摆一次
    reflow();

    // 默认选中第一个：呼出/输入即有落点，回车与方向键都从它起步。
    // 但这**不算**用户手动挑过（m_currentByUser 保持 false），所以回车仍然优先
    // 走建议条与就地求值 —— 见 activateCurrent 的两条短路。
    if (!m_tiles.isEmpty())
        setCurrentIndex(0);

    updateFooter();
}

void HomePage::updateIntent(const QString &query, bool hasToolHits)
{
    // 贴图优先于文字：图片已经把搜索框清空，此时 query 没有意义
    m_intentData = !m_pendingImage.isNull()
        ? queryintent::forImage(m_pendingImage.size())
        : queryintent::forText(query, hasToolHits);

    if (!m_intentData.valid()) {
        m_intent->hide();
        return;
    }

    // 目标插件没加载时（用户手动删过 plugins 目录里的文件）宁可不提示：
    // 摆一条回车没反应的建议比什么都不提示更糟。
    const ToolEntry *entry = m_registry ? m_registry->entryById(m_intentData.toolId) : nullptr;
    if (!entry) {
        m_intentData = queryintent::Suggestion{};
        m_intent->hide();
        return;
    }

    QString title;
    QString detail;
    switch (m_intentData.kind) {
    case queryintent::Kind::Json:
        title = QStringLiteral("用「%1」打开这段 JSON").arg(entry->name());
        detail = queryintent::preview(m_intentData.text);
        break;
    case queryintent::Kind::Image:
        title = QStringLiteral("用「%1」提取图片里的文字").arg(entry->name());
        detail = QStringLiteral("已粘贴 %1 × %2 的图片，回车直接识别")
                     .arg(m_intentData.imageSize.width())
                     .arg(m_intentData.imageSize.height());
        break;
    case queryintent::Kind::Translate:
        title = QStringLiteral("用「%1」翻译这段英文").arg(entry->name());
        detail = queryintent::preview(m_intentData.text);
        break;
    case queryintent::Kind::None:
        break;
    }

    m_intent->setContent(entry->icon, title, detail);
    m_intent->show();
}

void HomePage::activateIntent()
{
    if (!m_intentData.valid())
        return;

    xtools::ToolInput input;
    if (m_intentData.kind == queryintent::Kind::Image) {
        input.kind = xtools::ToolInput::Kind::Image;
        input.image = m_pendingImage;
    } else {
        input.kind = xtools::ToolInput::Kind::Text;
        input.text = m_intentData.text;
    }

    const QString toolId = m_intentData.toolId;

    // 交出去之后建议必须失效：否则从工具页 Esc 回首页时，focusSearch() 之前
    // 会先闪一下「已粘贴的图片」建议（图片已经被送走了，这条建议是假的）。
    clearPendingImage();
    m_intentData = queryintent::Suggestion{};
    m_intent->hide();

    emit toolActivatedWithInput(toolId, input);
}

void HomePage::setPendingImage(const QImage &image)
{
    if (image.isNull())
        return;

    m_pendingImage = image;

    // 清掉上一轮的文字关键词：留着会变成「既在搜文字又在贴图」，两组结果互相干扰。
    // clear() 会发 textChanged 触发重建，此时 m_pendingImage 已就位，正是要的状态。
    if (!m_search->text().isEmpty()) {
        m_search->clear();
    } else {
        m_searchClear->hide();
        rebuild();
    }
    m_search->setFocus(Qt::OtherFocusReason);
}

void HomePage::clearPendingImage()
{
    if (m_pendingImage.isNull())
        return;
    m_pendingImage = QImage();
    m_intentData = queryintent::Suggestion{};
    if (m_intent)
        m_intent->hide();
}

void HomePage::clearSearch()
{
    // 先清图片，再清文字：clear() 触发的重建就不该再带出贴图建议
    clearPendingImage();
    if (m_search->text().isEmpty()) {
        m_searchClear->hide();
        rebuild();
    } else {
        m_search->clear();   // textChanged 会触发重建并同步 ✕ 的显隐
    }
}

QImage HomePage::clipboardImage()
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard || !clipboard->mimeData() || !clipboard->mimeData()->hasImage())
        return QImage();

    QImage image = clipboard->image();
    if (image.isNull()) {
        // 部分截图工具只往剪贴板放 pixmap（没有 image 变体），再取一次兜底
        const QPixmap pixmap = clipboard->pixmap();
        if (!pixmap.isNull())
            image = pixmap.toImage();
    }
    return image;
}

// ---------------------------------------------------------------------------
// 就地求值：搜索框右侧的结果徽标
//
// 交互约定（README 里也有）：
//   * 输入能算的算式 → 框内出现「= 96」，不弹面板、不列磁贴、不抢焦点；
//   * 回车 / 点徽标 → 把结果整段复制到剪贴板，徽标短暂变成「已复制」；
//   * 结果变了 / 清空输入 → 徽标立刻消失，上一次的「已复制」反馈一并作废。
// ---------------------------------------------------------------------------

void HomePage::updateInlineResult()
{
    if (!m_inlineResult || !m_search)
        return;

    // 贴图时文字已被清空（见 setPendingImage），此时不该再有算式结果
    quickcalc::Result result;
    if (m_pendingImage.isNull() && !m_search->text().trimmed().isEmpty())
        result = quickcalc::evaluate(m_search->text());

    const QString value = result.ok ? result.text : QString();
    if (value != m_inlineValue) {
        m_inlineValue = value;
        // 结果换了，上一次的「已复制」必须立刻作废 —— 否则会把新结果说成已复制。
        // 这里直接改属性再 repolish，不走 setInlineFeedback()：那个函数会按旧的
        // m_inlineValue 重画一遍，等于多闪一次。
        m_inlineTimer->stop();
        if (m_inlineResult->property("feedback").toBool()) {
            m_inlineResult->setProperty("feedback", false);
            m_inlineResult->style()->unpolish(m_inlineResult);
            m_inlineResult->style()->polish(m_inlineResult);
        }
    }

    if (m_inlineValue.isEmpty()) {
        m_inlineResult->hide();
        // 徽标不在了，右边距收回：长表达式可以一直写到 ✕ 前面
        if (m_search->textMargins().right() != 0)
            m_search->setTextMargins(0, 0, 0, 0);
        return;
    }

    refreshInlineBadge();
    m_inlineResult->show();
    m_inlineResult->raise();
    positionSearchOverlays();
}

void HomePage::refreshInlineBadge()
{
    if (!m_inlineResult || m_inlineValue.isEmpty())
        return;

    const bool copied = m_inlineResult->property("feedback").toBool();
    const QString full = copied ? QStringLiteral("已复制")
                                : QStringLiteral("= %1").arg(m_inlineValue);

    // 字号与宽度都在代码里定：徽标是 QLineEdit 的子控件，几何手动控制，
    // 若让 qss 设 font-size，下面按 QFontMetrics 算出的宽度就对不上、文字会被裁。
    // 字体族沿用搜索框，视觉上与输入的文字同一套字。
    QFont font = m_search->font();
    font.setPixelSize(13);

    const QFontMetrics fm(font);
    const int textWidth = fm.horizontalAdvance(full);
    const int maxTextWidth = kInlineResultMaxWidth - 2 * kInlineResultPad;
    // 结果过长（2^1000 这类）按像素省略；复制走的仍是完整值，完整值放 tooltip
    const QString shown = textWidth > maxTextWidth
        ? fm.elidedText(full, Qt::ElideRight, maxTextWidth)
        : full;

    m_inlineResult->setFont(font);
    m_inlineResult->setText(shown);
    m_inlineResult->setFixedSize(qMin(textWidth, maxTextWidth) + 2 * kInlineResultPad,
                                 fm.height() + 2);
    m_inlineResult->setToolTip(copied ? QString()
                                      : QStringLiteral("%1\n回车或点击复制").arg(full));
}

void HomePage::positionSearchOverlays()
{
    if (!m_search)
        return;

    // ✕ 贴右内缘；徽标紧挨在 ✕ 左侧
    int clearX = m_search->width() - kSearchClearGap;
    if (m_searchClear) {
        clearX = m_search->width() - kSearchClearGap - m_searchClear->width();
        m_searchClear->move(clearX, (m_search->height() - m_searchClear->height()) / 2);
        m_searchClear->raise();
    }

    if (m_inlineResult && m_inlineResult->isVisible()) {
        // 窗口很窄时不让徽标越过文字起始处（左侧 padding），宁可它盖住 ✕
        const int x = qMax(kSearchTextPad, clearX - kInlineGap - m_inlineResult->width());
        m_inlineResult->move(x, (m_search->height() - m_inlineResult->height()) / 2);
        m_inlineResult->raise();

        // 给右侧让出徽标的宽度，否则正在输入的表达式会钻到徽标底下。
        // 比较后再设：setTextMargins 每次都会让 QLineEdit 重算几何，
        // 在 resize 里无脑调用容易变成「改边距 → 重排 → 再改」的循环。
        const int wanted = m_inlineResult->width() + 2 * kInlineGap;
        if (m_search->textMargins().right() != wanted)
            m_search->setTextMargins(0, 0, wanted, 0);
    }
}

void HomePage::setInlineFeedback(bool on)
{
    if (!m_inlineResult || m_inlineResult->property("feedback").toBool() == on)
        return;

    m_inlineResult->setProperty("feedback", on);
    // 动态属性变了必须 unpolish/polish，qss 的 [feedback="true"] 才会重新求值
    // （与 LauncherTile 的 hover/current 同一套路）
    m_inlineResult->style()->unpolish(m_inlineResult);
    m_inlineResult->style()->polish(m_inlineResult);

    // 文字在「= 96」与「已复制」之间切换，宽度会变，位置/边距都要跟着重算
    refreshInlineBadge();
    positionSearchOverlays();
}

void HomePage::copyInlineResult()
{
    if (m_inlineValue.isEmpty())
        return;

    QGuiApplication::clipboard()->setText(m_inlineValue);
    setInlineFeedback(true);
    m_inlineTimer->start();   // 到点自动复位成「= 结果」

    // 焦点留在搜索框：复制完通常还要接着改算式，不该把用户的手赶走
    m_search->setFocus(Qt::OtherFocusReason);
}

void HomePage::showEmptyState(bool empty)
{
    m_top->setVisible(!empty);
    m_emptyBox->setVisible(empty);
    if (!empty)
        return;   // 非空态的分组显隐由 rebuild() 按实际内容决定

    m_recentBox->hide();
    m_allBox->hide();
    m_appBox->hide();
    m_intent->hide();
    m_noResult->hide();
}

void HomePage::updateFooter()
{
    const int tools = m_registry ? m_registry->count() : 0;
    const int apps = m_apps ? m_apps->count() : 0;
    const int failed = m_plugins ? m_plugins->errors().size() : 0;

    QStringList parts;
    parts.append(QStringLiteral("%1 个工具").arg(tools));
    if (apps > 0)
        parts.append(QStringLiteral("%1 个本机应用").arg(apps));
    if (failed > 0)
        parts.append(QStringLiteral("%1 个失败").arg(failed));

    m_footer->setText(QStringLiteral("已加载 ") + parts.join(QStringLiteral(" · ")));

    // 提示跟着角标一起显隐：只有一个结果时回车就是它，提数字键反而是噪音；
    // 文案随「搜索框有没有字」变 —— 有字时裸数字是输入，必须按住 Ctrl 才是直选。
    // 方向键提示只在有多项时才有意义（一项时上下左右没有可去的目标）。
    const bool numbered = m_tiles.size() >= 2;
    m_shortcutHint->setVisible(numbered);
    if (numbered) {
        const QString direct = m_search->text().isEmpty()
            ? QStringLiteral("数字键 1-9 直接打开")
            : QStringLiteral("Ctrl+1-9 直接打开");
        // 把「方向键能选」写出来：高亮只有按下方向键才出现，不写用户不会知道
        m_shortcutHint->setText(direct + QStringLiteral(" · 方向键选择"));
    }
}

int HomePage::columnsFor(int viewportWidth) const
{
    const int avail = viewportWidth - 2 * xtools::layout::kPageMargin - kScrollbarReserve;
    return qMax(1, (avail + kGridSpacing) / (kTileWidth + kGridSpacing));
}

void HomePage::placeTiles(QGridLayout *grid, const QList<LauncherTile *> &tiles, int columns)
{
    // 先摘再放：对「已经在本布局里的控件」直接 addWidget 换格子，结果依赖 Qt
    // 内部实现（可能保留旧格子）。removeWidget 之后重新 addWidget 才是确定的。
    // 注意 removeWidget 会把控件 hide()，所以放回去必须显式 show()。
    for (LauncherTile *tile : tiles)
        grid->removeWidget(tile);

    for (int i = 0; i < tiles.size(); ++i) {
        grid->addWidget(tiles.at(i), i / columns, i % columns, Qt::AlignLeft | Qt::AlignTop);
        tiles.at(i)->show();
    }
}

void HomePage::reflow()
{
    const int columns = columnsFor(m_scroll->viewport()->width());
    if (columns == m_lastColumns)
        return;   // 列数没变就不动布局，拖动窗口时才不会闪
    m_lastColumns = columns;
    placeTiles(m_recentGrid, m_recentTiles, columns);
    placeTiles(m_allGrid, m_allTiles, columns);
    placeTiles(m_appGrid, m_appTiles, columns);
}

void HomePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    reflow();
}

void HomePage::focusSearch()
{
    // 关键词和待处理图片都要清掉：热键呼出是「重新开始一次搜索」的语义，
    // 残留的 JSON 或图片会让下次呼出直接落到上一次的建议上。
    if (!m_search->text().isEmpty() || !m_pendingImage.isNull())
        clearSearch();   // textChanged / 手动调用都会重建
    else
        rebuild();       // 关键词本来就是空的，信号不会发，手动刷一次
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();   // 全选：直接输入即替换旧关键词
}

void HomePage::setSearchText(const QString &text)
{
    clearPendingImage();       // 命令行预填只处理文字，图片建议要失效
    m_search->setText(text);   // textChanged 触发重建
    m_search->setFocus(Qt::OtherFocusReason);
    m_search->selectAll();
}

int HomePage::columnCount() const
{
    return columnsFor(m_scroll->viewport()->width());
}

void HomePage::applyShortcutNumbers()
{
    // 只有一个结果时不标号：此时回车打开的就是它，标个「1」只会让人以为还得按一下。
    // 第 10 项起不标号（键盘没有 10 这个直选位），它们仍能用方向键 + 回车打开。
    const bool numbered = m_tiles.size() >= 2;
    for (int i = 0; i < m_tiles.size(); ++i)
        m_tiles.at(i)->setShortcutNumber(numbered && i < kMaxShortcut ? i + 1 : 0);
}

bool HomePage::activateNumber(int number)
{
    if (number < 1 || number > kMaxShortcut || number > m_tiles.size())
        return false;

    LauncherTile *tile = m_tiles.at(number - 1);
    // 先把高亮落到这个磁贴上再打开：滚动容器会顺手把它滚进可视区，
    // 万一打开失败（比如应用被删了）用户也能看见刚才选的是哪一个。
    setCurrentIndex(number - 1);
    activateTile(tile);
    return true;
}

void HomePage::activateTile(LauncherTile *tile)
{
    if (!tile)
        return;
    if (tile->kind() == LauncherItem::Kind::App)
        emit appActivated(tile->id());
    else
        emit toolActivated(tile->id());
}

void HomePage::setCurrentIndex(int index)
{
    if (m_tiles.isEmpty())
        return;
    // 负数代表「这一项不在当前网格里」（例如点击后立刻重建过），不要钳到 0 上：
    // 那会让高亮莫名其妙地跳到第一项。
    if (index < 0)
        return;

    index = qBound(0, index, m_tiles.size() - 1);
    if (m_currentIndex >= 0 && m_currentIndex < m_tiles.size())
        m_tiles.at(m_currentIndex)->setCurrent(false);

    m_currentIndex = index;
    m_tiles.at(index)->setCurrent(true);
    scrollTileIntoView(m_tiles.at(index));
}

void HomePage::scrollTileIntoView(LauncherTile *tile)
{
    if (!m_scroll || !tile)
        return;

    QWidget *viewport = m_scroll->viewport();
    const QRect target(tile->mapTo(viewport, QPoint(0, 0)), tile->size());
    const QRect visible = viewport->rect();

    // 已经完整可见就什么都不做。以前每次移动都调 ensureWidgetVisible()，它会把
    // 目标「贴到」视口边缘、还带一点内缩，于是每按一下滚动条都要微调几像素，
    // 整片网格跟着挪一下 —— 视觉上就是抖，而不是一格格干净地跳。
    if (visible.contains(target))
        return;

    QScrollBar *bar = m_scroll->verticalScrollBar();
    int value = bar->value();
    if (target.top() < visible.top())
        value -= visible.top() - target.top();      // 目标在上方：滚到它顶边对齐
    else if (target.bottom() > visible.bottom())
        value += target.bottom() - visible.bottom(); // 目标在下方：滚到它底边对齐
    bar->setValue(value);
}

void HomePage::selectTileFromMouse(LauncherTile *tile)
{
    if (!tile)
        return;
    const int index = m_tiles.indexOf(tile);   // 不在当前网格里时 indexOf 给 -1，会被忽略
    if (index < 0)
        return;
    m_currentByUser = true;   // 鼠标点过 = 明确挑了这一项，回车按它走
    setCurrentIndex(index);
}

QList<QList<LauncherTile *>> HomePage::tileGroups() const
{
    // 只列「真正显示出来」的组：空组在 rebuild() 里已被 hide()，把它算进导航
    // 会让方向键停在一个看不见的位置上。
    QList<QList<LauncherTile *>> groups;
    if (!m_recentTiles.isEmpty())
        groups.append(m_recentTiles);
    if (!m_allTiles.isEmpty())
        groups.append(m_allTiles);
    if (!m_appTiles.isEmpty())
        groups.append(m_appTiles);
    return groups;
}

void HomePage::moveCurrentBy(int dRow, int dCol)
{
    if (m_tiles.isEmpty())
        return;

    // 从这一下起就是「用户自己挑的」：回车按选中项走，建议条与就地求值让位。
    // 默认的「选中第一个」不设这个标记，否则回车永远只会打开第一项。
    m_currentByUser = true;

    const QList<QList<LauncherTile *>> groups = tileGroups();
    if (groups.isEmpty())
        return;

    // 第一次按键只「点亮」不移动：往下/往右从第一项起，往上/往左从最后一项起。
    // 这样按一下 ↑ 不会先跳到倒数第一项再往上（那一下就白按了）。
    if (m_currentIndex < 0) {
        setCurrentIndex((dRow < 0 || dCol < 0) ? m_tiles.size() - 1 : 0);
        return;
    }

    // 定位当前项：先找它在哪个组，再换算成组内序号（m_tiles = 各组直接拼接）
    int groupIndex = -1;
    int groupStart = 0;
    for (int i = 0; i < groups.size(); ++i) {
        const int n = groups.at(i).size();
        if (m_currentIndex >= groupStart && m_currentIndex < groupStart + n) {
            groupIndex = i;
            break;
        }
        groupStart += n;
    }
    if (groupIndex < 0)
        return;   // 与 m_tiles 对不上（理论上不会发生），保守不动

    // 左右：沿视觉顺序走一格，跨组时直接接到相邻组的第一项 / 最后一项。
    // 分组只是视觉上的分隔，对「一格一格挪」的用户来说仍是一条连续的线。
    if (dCol != 0) {
        setCurrentIndex(qBound(0, m_currentIndex + dCol, m_tiles.size() - 1));
        return;
    }

    const int columns = qMax(1, columnCount());
    const int count = groups.at(groupIndex).size();
    const int row = (m_currentIndex - groupStart) / columns;
    const int col = (m_currentIndex - groupStart) % columns;
    const int rows = (count + columns - 1) / columns;

    if (dRow > 0) {
        if (row + 1 < rows) {
            // 同一组的下一行：列数对齐；下一行不满（末行）时退到它的最后一项，
            // 否则按下键会落进空位、表现成「按了没反应」
            setCurrentIndex(groupStart + qMin((row + 1) * columns + col, count - 1));
            return;
        }
        // 到组尾：进下一组的第一行，列尽量对齐（那组不足一行时就是它的最后一项）
        if (groupIndex + 1 < groups.size()) {
            const int nextStart = groupStart + count;
            setCurrentIndex(nextStart + qMin(col, groups.at(groupIndex + 1).size() - 1));
        }
        // 已经是整个网格的最后一行：原地不动，不做「回卷到顶部」
        return;
    }

    if (row > 0) {
        setCurrentIndex(groupStart + (row - 1) * columns + col);
        return;
    }
    // 到组首：进上一组的最后一行，列尽量对齐（末行不满时退到它的最后一项）
    if (groupIndex > 0) {
        const int prevCount = groups.at(groupIndex - 1).size();
        const int prevStart = groupStart - prevCount;
        const int prevRows = (prevCount + columns - 1) / columns;
        const int prevLastRowCount = prevCount - (prevRows - 1) * columns;
        setCurrentIndex(prevStart + (prevRows - 1) * columns + qMin(col, prevLastRowCount - 1));
    }
}

bool HomePage::activateCurrent()
{
    // 就地求值优先于一切：答案已经摆在那行了，回车就是「把这个数拿走」，
    // 而不是替用户打开一个他并不需要的面板（这就是「不用打开面板」的落点）。
    // 判据是「用户有没有自己挑过磁贴」，不是「当前下标是否有效」—— 首页默认就
    // 选中第一项，用下标判断会让这两条捷径直接失效。
    if (!m_inlineValue.isEmpty() && !m_currentByUser) {
        copyInlineResult();
        return true;
    }

    // 建议条优先：用户还没用方向键挑具体磁贴时，回车就是「按建议办」。
    // 一旦挑过（方向键或点击），说明他明确选中了某个磁贴，按磁贴走。
    if (m_intentData.valid() && !m_currentByUser) {
        activateIntent();
        return true;
    }

    if (m_tiles.isEmpty())
        return false;

    const int index = m_currentIndex >= 0 ? m_currentIndex : 0;
    activateTile(m_tiles.at(index));
    return true;
}

bool HomePage::eventFilter(QObject *watched, QEvent *event)
{
    // 视口尺寸变化 = 可用宽度变了，列数要重算。
    // 滚动条出现/消失也会走到这里，但列数公式恒定预留了滚动条宽度，算出来不会变，
    // reflow() 的「列数没变就 return」会直接短路，不会反复重排。
    if (m_scroll && watched == m_scroll->viewport() && event->type() == QEvent::Resize) {
        reflow();
        return false;
    }

    // 结果徽标：单击 = 复制（与回车同一动作）。双击要吞掉 —— 否则会冒泡成
    // QLineEdit 的双击选词，把徽标位置上的文字选中，看着像复制到了别的东西。
    if (m_inlineResult && watched == m_inlineResult) {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton
                && m_inlineResult->rect().contains(me->position().toPoint())) {
                copyInlineResult();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_search) {
        // 清空按钮与结果徽标都挂在搜索框上，几何完全手动控制，尺寸一变就重摆
        if (event->type() == QEvent::Resize) {
            positionSearchOverlays();
            return false;
        }

        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);

            // 剪贴板里是图片时，Ctrl+V / Shift+Insert 走「贴图」路径。
            // 直接交给 QLineEdit 只会取文本，图片被它静默丢掉（用户会以为复制失败）。
            // 不是图片就原样放行，仍由 QLineEdit 完成普通粘贴。
            const bool pasteChord = (ke->key() == Qt::Key_V
                                     && ke->modifiers().testFlag(Qt::ControlModifier))
                                    || (ke->key() == Qt::Key_Insert
                                        && ke->modifiers().testFlag(Qt::ShiftModifier));
            if (pasteChord) {
                const QImage image = clipboardImage();
                if (!image.isNull()) {
                    setPendingImage(image);
                    return true;
                }
            }

            // 数字键直选（1~9）。为什么分成两条通道：
            //   * 搜索框为空（热键刚呼出的状态）时，裸数字直接打开第 N 项 —— 这是
            //     最顺手的动作，不该逼用户先按修饰键；
            //   * 搜索框有字时，裸数字必须留给输入（否则 base64 / 360 / 7z 这类
            //     关键词一打就断），直选改用 Ctrl+数字。
            // 比较修饰键前清掉 KeypadModifier：小键盘数字也带这个标志，不清掉的话
            // 按小键盘 1 匹配不上「无修饰键」，会静默失效。
            // 序号没有对应磁贴时一律放行，让数字照常进搜索框 —— 宁可多打一个字，
            // 也不要出现「按了完全没反应」的死键。
            const int key = ke->key();
            if (key >= Qt::Key_1 && key <= Qt::Key_9) {
                const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
                const bool withCtrl = (mods == Qt::ControlModifier);
                const bool bare = (mods == Qt::NoModifier) && m_search->text().isEmpty();
                if ((withCtrl || bare) && activateNumber(key - Qt::Key_0))
                    return true;
            }

            // 方向键只在「裸按」时接管选中项：带 Shift/Ctrl/Alt 的组合留给
            // QLineEdit（Ctrl+←/→ 是整词跳、Shift+←/→ 是扩选），抢走会让编辑失能。
            // 比较前清掉 KeypadModifier：小键盘方向键会带这个标志。
            const Qt::KeyboardModifiers arrowMods = ke->modifiers() & ~Qt::KeypadModifier;
            const bool plainArrow = (arrowMods == Qt::NoModifier);

            switch (ke->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                activateCurrent();
                return true;   // 没结果也吞掉，避免冒泡触发别处的默认动作
            case Qt::Key_Right:
            case Qt::Key_Left: {
                const bool forward = (ke->key() == Qt::Key_Right);
                if (!plainArrow)
                    break;   // Ctrl+←/→、Shift+←/→ 交回 QLineEdit
                // 左右键同时是「移光标」和「换选中项」，按状态分：
                //   * 用户已经自己挑过磁贴 → 正在挑东西，左右换项；
                //   * 搜索框是空的 → 没有文字可编辑，左右直接换项；
                //   * 光标还能往这个方向挪 → 先让它挪。改错字比换选中项急，
                //     而且「挪到尽头再按一下」本来就是一句很自然的动作；
                //   * 光标已经顶到头 → 这一下已经没有编辑意义，用来换项。
                if (!m_currentByUser && !m_search->text().isEmpty()) {
                    const int caret = m_search->cursorPosition();
                    const int length = m_search->text().length();
                    if (forward ? caret < length : caret > 0)
                        break;   // 放行给 QLineEdit
                }
                moveCurrentBy(0, forward ? 1 : -1);
                return true;
            }
            case Qt::Key_Down:
                if (!plainArrow)
                    break;
                moveCurrentBy(1, 0);
                return true;
            case Qt::Key_Up:
                if (!plainArrow)
                    break;
                moveCurrentBy(-1, 0);
                return true;
            case Qt::Key_Escape:
                if (m_search->text().isEmpty() && m_pendingImage.isNull())
                    break;   // 没可清空的内容：交回传播链，由 MainWindow 决定收托盘
                clearSearch();
                return true;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

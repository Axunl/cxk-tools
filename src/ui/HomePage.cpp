#include "HomePage.h"

#include "IconBadge.h"
#include "PluginManager.h"
#include "Settings.h"
#include "ToolRegistry.h"

#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// 首页布局常量。与 MainWindow 的页头左右留白保持一致（28），换页时标题不会跳。
constexpr int kPageMargin = 28;
constexpr int kTileWidth = 104;
constexpr int kTileHeight = 100;
constexpr int kTileIcon = 48;
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
constexpr int kNameMaxWidth = kTileWidth - 12;

} // namespace

// ---------------------------------------------------------------------------
// ToolTile
// ---------------------------------------------------------------------------

ToolTile::ToolTile(const ToolEntry &entry, QWidget *parent)
    : QWidget(parent)
    , m_id(entry.info.id)
    , m_fullName(entry.name())
    , m_icon(entry.icon)
{
    setObjectName(QStringLiteral("toolTile"));
    // QWidget 子类的 qss 背景必须靠 WA_StyledBackground 才绘制，
    // 不设的话 hover/current 的底色完全看不到（只剩文字和图标）。
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedSize(kTileWidth, kTileHeight);
    setCursor(Qt::PointingHandCursor);
    setToolTip(entry.info.description);

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
}

void ToolTile::setCurrent(bool on)
{
    if (m_current == on)
        return;
    m_current = on;
    setProperty("current", on);
    // 动态属性变了必须 unpolish/polish，qss 选择器才会重新求值
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void ToolTile::setHovered(bool on)
{
    if (m_hovered == on)
        return;
    m_hovered = on;
    setProperty("hovered", on);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void ToolTile::updateNameText()
{
    if (!m_name)
        return;
    // 名称超长就省略：磁贴固定宽度，QLabel 不会自己截断，会直接溢出到邻居上
    const QFontMetrics fm(m_name->font());
    m_name->setText(fm.elidedText(m_fullName, Qt::ElideRight, kNameMaxWidth));
    m_name->setToolTip(m_fullName);
}

void ToolTile::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    // 图标按真实 DPR 取：构造时窗口还没落到屏幕上，devicePixelRatioF() 不可靠
    QPixmap pm = m_icon.pixmap(QSize(kTileIcon, kTileIcon), devicePixelRatioF());
    if (pm.isNull())   // 插件图标文件坏了或为空时兜底，绝不留一块空白
        pm = IconBadge::pixmap(m_id, m_fullName, kTileIcon, devicePixelRatioF());
    m_iconLabel->setPixmap(pm);

    // 字体（qss 里的 12px）此时才生效，省略宽度要按最终字体算
    updateNameText();
}

void ToolTile::mouseReleaseEvent(QMouseEvent *event)
{
    // 只认「在磁贴内按下并抬起」的左键；右键交给 contextMenuEvent
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        emit activated(m_id);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ToolTile::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *popOut = menu.addAction(QStringLiteral("在独立窗口打开"));
    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == popOut)
        emit popOutRequested(m_id);
    event->accept();
}

void ToolTile::enterEvent(QEnterEvent *event)
{
    setHovered(true);
    QWidget::enterEvent(event);
}

void ToolTile::leaveEvent(QEvent *event)
{
    setHovered(false);
    QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
// HomePage
// ---------------------------------------------------------------------------

HomePage::HomePage(ToolRegistry *registry, Settings *settings, PluginManager *plugins, QWidget *parent)
    : QWidget(parent)
    , m_registry(registry)
    , m_settings(settings)
    , m_plugins(plugins)
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
    topLayout->setContentsMargins(kPageMargin, 18, kPageMargin, 6);
    topLayout->setSpacing(0);

    m_search = new QLineEdit(m_top);
    m_search->setObjectName(QStringLiteral("homeSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索工具，回车打开…"));
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
    connect(m_searchClear, &QToolButton::clicked, m_search, &QLineEdit::clear);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_searchClear->setVisible(!text.isEmpty());
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
    contentLayout->setContentsMargins(kPageMargin, 4, kPageMargin, 16);
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

    // 搜索无命中：与「一个工具都没有」是两回事，文案与处理都分开
    m_noResult = new QLabel(QStringLiteral("没有找到匹配的工具"), m_content);
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

    contentLayout->addWidget(m_recentBox);
    contentLayout->addWidget(m_allBox);
    contentLayout->addWidget(m_noResult);
    contentLayout->addWidget(m_emptyBox);
    contentLayout->addStretch(1);

    m_footer = new QLabel(this);
    m_footer->setObjectName(QStringLiteral("homeFooter"));

    layout->addWidget(m_top);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_footer);
}

void HomePage::clearTiles()
{
    // 立即删除而不是 deleteLater：紧接着就要重建并 addWidget，
    // 延迟删除会让旧 QWidgetItem 短暂留在布局里，重排结果不可预期。
    for (ToolTile *tile : m_tiles)
        delete tile;
    m_tiles.clear();
    m_recentTiles.clear();
    m_allTiles.clear();
    m_currentIndex = -1;
}

void HomePage::rebuild()
{
    clearTiles();

    const QString query = m_search->text().trimmed();

    if (m_registry->count() == 0) {
        showEmptyState(true);
        updateFooter();
        return;
    }
    showEmptyState(false);

    auto addTile = [this](const ToolEntry &entry, QList<ToolTile *> *bucket) {
        auto *tile = new ToolTile(entry, m_content);
        connect(tile, &ToolTile::activated, this, &HomePage::toolActivated);
        connect(tile, &ToolTile::popOutRequested, this, &HomePage::toolPopOutRequested);
        bucket->append(tile);
    };

    if (query.isEmpty()) {
        m_recentBox->show();
        m_allTitle->setText(QStringLiteral("全部"));

        // 最近使用：键里可能残留已删除的插件 id，读不到条目就跳过
        const QStringList recent = m_settings->recentToolIds();
        for (const QString &id : recent) {
            if (const ToolEntry *entry = m_registry->entryById(id))
                addTile(*entry, &m_recentTiles);
        }

        // 「全部」用加载顺序而不是使用频次：网格比列表更依赖位置记忆，
        // 按频次排会让磁贴随使用漂移，找东西全靠扫。
        for (const ToolEntry &entry : m_registry->entries())
            addTile(entry, &m_allTiles);

        m_recentBox->setVisible(!m_recentTiles.isEmpty());
    } else {
        m_recentBox->hide();
        m_allTitle->setText(QStringLiteral("搜索结果"));
        // 搜索走相关度排序，第一个就是回车要打开的那个
        const QStringList hits = m_registry->search(query, 1000);
        for (const QString &id : hits) {
            if (const ToolEntry *entry = m_registry->entryById(id))
                addTile(*entry, &m_allTiles);
        }
    }

    m_tiles = m_recentTiles + m_allTiles;
    m_noResult->setVisible(!query.isEmpty() && m_allTiles.isEmpty());
    m_allBox->setVisible(!m_allTiles.isEmpty());

    m_lastColumns = -1;   // 磁贴全换了，强制重新摆一次
    reflow();
    updateFooter();
}

void HomePage::showEmptyState(bool empty)
{
    m_top->setVisible(!empty);
    m_recentBox->setVisible(!empty && !m_recentTiles.isEmpty());
    m_allBox->setVisible(!empty);
    m_emptyBox->setVisible(empty);
    m_noResult->hide();
}

void HomePage::updateFooter()
{
    const int n = m_registry->count();
    const int failed = m_plugins ? m_plugins->errors().size() : 0;
    m_footer->setText(failed > 0
                          ? QStringLiteral("已加载 %1 个工具 · %2 个失败").arg(n).arg(failed)
                          : QStringLiteral("已加载 %1 个工具").arg(n));
}

int HomePage::columnsFor(int viewportWidth) const
{
    const int avail = viewportWidth - 2 * kPageMargin - kScrollbarReserve;
    return qMax(1, (avail + kGridSpacing) / (kTileWidth + kGridSpacing));
}

void HomePage::placeTiles(QGridLayout *grid, const QList<ToolTile *> &tiles, int columns)
{
    // 先摘再放：对「已经在本布局里的控件」直接 addWidget 换格子，结果依赖 Qt
    // 内部实现（可能保留旧格子）。removeWidget 之后重新 addWidget 才是确定的。
    // 注意 removeWidget 会把控件 hide()，所以放回去必须显式 show()。
    for (ToolTile *tile : tiles)
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
}

void HomePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    reflow();
}

void HomePage::focusSearch()
{
    if (!m_search->text().isEmpty())
        m_search->clear();   // textChanged 会触发重建
    else
        rebuild();           // 关键词本来就是空的，信号不会发，手动刷一次
    m_search->setFocus(Qt::ShortcutFocusReason);
    m_search->selectAll();   // 全选：直接输入即替换旧关键词
}

int HomePage::columnCount() const
{
    return columnsFor(m_scroll->viewport()->width());
}

void HomePage::setCurrentIndex(int index)
{
    if (m_tiles.isEmpty())
        return;

    index = qBound(0, index, m_tiles.size() - 1);
    if (m_currentIndex >= 0 && m_currentIndex < m_tiles.size())
        m_tiles.at(m_currentIndex)->setCurrent(false);

    m_currentIndex = index;
    m_tiles.at(index)->setCurrent(true);
    m_scroll->ensureWidgetVisible(m_tiles.at(index), 8, 8);
}

void HomePage::moveCurrent(int delta)
{
    if (m_tiles.isEmpty() || delta == 0)
        return;
    if (m_currentIndex < 0)
        setCurrentIndex(delta > 0 ? 0 : m_tiles.size() - 1);
    else
        setCurrentIndex(m_currentIndex + delta);
}

bool HomePage::activateCurrent()
{
    if (m_tiles.isEmpty())
        return false;
    const int index = m_currentIndex >= 0 ? m_currentIndex : 0;
    emit toolActivated(m_tiles.at(index)->toolId());
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

    if (watched == m_search) {
        // 清空按钮跟随搜索框尺寸摆位：它是搜索框的子控件，几何完全手动控制
        if (event->type() == QEvent::Resize) {
            if (m_searchClear) {
                m_searchClear->move(m_search->width() - kSearchClearGap - m_searchClear->width(),
                                    (m_search->height() - m_searchClear->height()) / 2);
                m_searchClear->raise();
            }
            return false;
        }

        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            switch (ke->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                activateCurrent();
                return true;   // 没结果也吞掉，避免冒泡触发别处的默认动作
            case Qt::Key_Right:
                moveCurrent(1);
                return true;
            case Qt::Key_Left:
                moveCurrent(-1);
                return true;
            case Qt::Key_Down:
                moveCurrent(columnCount());
                return true;
            case Qt::Key_Up:
                moveCurrent(-columnCount());
                return true;
            case Qt::Key_Escape:
                if (m_search->text().isEmpty())
                    break;   // 没可清空的内容：交回传播链，由 MainWindow 决定收托盘
                m_search->clear();
                return true;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

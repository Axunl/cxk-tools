#include "MainWindow.h"

#include "IconBadge.h"
#include "PluginManager.h"
#include "Settings.h"
#include "SettingsPage.h"
#include "ToolRegistry.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSizeGrip>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace {
// 布局常量集中在这里：改一处即可，避免魔法数字散落
constexpr int kTitleHeight = 46;
constexpr int kSidebarWidth = 208;
constexpr int kIconSize = 20;
} // namespace

MainWindow::MainWindow(ToolRegistry *registry, Settings *settings, PluginManager *plugins, QWidget *parent)
    : QMainWindow(parent)
    , m_registry(registry)
    , m_settings(settings)
    , m_plugins(plugins)
{
    // 无边框 + 自绘标题栏（与 uTools 那类工具面板观感一致）。
    // 拖动靠 QWindow::startSystemMove()，缩放靠 QSizeGrip，都是跨平台 API。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("CXK 工具箱"));
    setMinimumSize(920, 580);
    resize(1120, 720);

    buildUi();
    refreshSidebar();
}

void MainWindow::buildUi()
{
    auto *root = new QWidget(this);
    root->setObjectName(QStringLiteral("mainRoot"));
    setCentralWidget(root);

    auto *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ---------------------------------------------------------------- 标题栏
    m_titleBar = new QWidget(root);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(kTitleHeight);
    m_titleBar->installEventFilter(this);   // 按住标题栏拖动窗口

    auto *tb = new QHBoxLayout(m_titleBar);
    tb->setContentsMargins(14, 0, 10, 0);
    tb->setSpacing(8);

    auto *logo = new QLabel(m_titleBar);
    logo->setFixedSize(22, 22);
    logo->setPixmap(IconBadge::pixmap(QStringLiteral("cxk-tools"), QStringLiteral("C"), 22, devicePixelRatioF()));

    auto *title = new QLabel(QStringLiteral("CXK 工具箱"), m_titleBar);
    title->setObjectName(QStringLiteral("titleText"));

    m_hotkeyHint = new QLabel(m_settings->hotkey().toString(QKeySequence::NativeText), m_titleBar);
    m_hotkeyHint->setObjectName(QStringLiteral("titleHotkey"));

    auto *minBtn = new QPushButton(QStringLiteral("\u2014"), m_titleBar);   // —
    minBtn->setObjectName(QStringLiteral("titleBtn"));
    minBtn->setFixedSize(30, 28);
    minBtn->setCursor(Qt::PointingHandCursor);

    auto *closeBtn = new QPushButton(QStringLiteral("\u2715"), m_titleBar);   // ✕
    closeBtn->setObjectName(QStringLiteral("titleBtnClose"));
    closeBtn->setFixedSize(30, 28);
    closeBtn->setCursor(Qt::PointingHandCursor);

    tb->addWidget(logo);
    tb->addWidget(title);
    tb->addSpacing(10);
    tb->addWidget(m_hotkeyHint);
    tb->addStretch(1);
    tb->addWidget(minBtn);
    tb->addWidget(closeBtn);

    connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);   // closeEvent 会改成 hide

    rootLayout->addWidget(m_titleBar);

    // ---------------------------------------------------------------- 主体
    auto *content = new QWidget(root);
    auto *contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    // 侧边栏
    auto *side = new QWidget(content);
    side->setObjectName(QStringLiteral("sidebar"));
    side->setFixedWidth(kSidebarWidth);

    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 10, 0, 10);
    sideLayout->setSpacing(8);

    m_searchEdit = new QLineEdit(side);
    m_searchEdit->setObjectName(QStringLiteral("sideSearch"));
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索工具…"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->installEventFilter(this);   // 回车/上下/Esc
    sideLayout->addWidget(m_searchEdit);

    m_sideList = new QListWidget(side);
    m_sideList->setObjectName(QStringLiteral("sideList"));
    m_sideList->setFrameShape(QFrame::NoFrame);
    m_sideList->setIconSize(QSize(kIconSize, kIconSize));
    m_sideList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sideList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_footer = new QLabel(side);
    m_footer->setObjectName(QStringLiteral("sideFooter"));
    m_footer->setAlignment(Qt::AlignCenter);

    sideLayout->addWidget(m_sideList, 1);
    sideLayout->addWidget(m_footer);

    // 右侧：页头 + 面板栈
    auto *right = new QWidget(content);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    auto *header = new QWidget(right);
    header->setObjectName(QStringLiteral("pageHeader"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(28, 16, 28, 12);
    headerLayout->setSpacing(2);

    m_toolTitle = new QLabel(header);
    m_toolTitle->setObjectName(QStringLiteral("toolTitle"));
    m_toolDesc = new QLabel(header);
    m_toolDesc->setObjectName(QStringLiteral("toolDesc"));
    headerLayout->addWidget(m_toolTitle);
    headerLayout->addWidget(m_toolDesc);

    m_stack = new QStackedWidget(right);
    m_stack->setObjectName(QStringLiteral("toolStack"));

    // 空状态页：一个工具都没加载到时显示，给一条"去哪放插件"的指引
    m_emptyPage = new QWidget(m_stack);
    auto *emptyLayout = new QVBoxLayout(m_emptyPage);
    emptyLayout->setContentsMargins(40, 40, 40, 40);
    emptyLayout->setSpacing(12);
    emptyLayout->addStretch(1);

    auto *emptyTitle = new QLabel(QStringLiteral("还没有加载到任何工具"), m_emptyPage);
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyTitle->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;color:#0F172A;"));

    auto *emptyDesc = new QLabel(
        QStringLiteral("把编译好的插件动态库放进 plugins 目录，然后重启应用。\n"
                       "想自己写一个工具，看 docs/PLUGIN.md（不到 30 行就能上手）。"),
        m_emptyPage);
    emptyDesc->setAlignment(Qt::AlignCenter);
    emptyDesc->setStyleSheet(QStringLiteral("font-size:12px;color:#94A3B8;"));

    auto *openDirBtn = new QPushButton(QStringLiteral("打开插件目录"), m_emptyPage);
    openDirBtn->setProperty("accent", true);
    openDirBtn->setFixedWidth(140);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(openDirBtn);
    btnRow->addStretch(1);

    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyDesc);
    emptyLayout->addLayout(btnRow);
    emptyLayout->addStretch(1);

    connect(openDirBtn, &QPushButton::clicked, this, [this] {
        const QString dir = m_plugins ? m_plugins->pluginDir() : QString();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    m_stack->addWidget(m_emptyPage);

    // 设置页也塞进同一个栈：这样切换设置与工具共用页头，不另开窗口
    m_settingsPage = new SettingsPage(m_settings, m_plugins, m_stack);
    m_stack->addWidget(m_settingsPage);

    connect(m_settingsPage, &SettingsPage::restartRequested, this, &MainWindow::restartRequested);
    connect(m_settingsPage, &SettingsPage::hotkeyChanged, this, [this](const QKeySequence &seq) {
        // 标题栏那行提示要跟着变，否则用户改完热键看到的还是旧组合
        if (m_hotkeyHint)
            m_hotkeyHint->setText(seq.toString(QKeySequence::NativeText));
        emit hotkeyChanged(seq);
    });

    rightLayout->addWidget(header);
    rightLayout->addWidget(m_stack, 1);

    contentLayout->addWidget(side);
    contentLayout->addWidget(right, 1);
    rootLayout->addWidget(content, 1);

    // 无边框窗口右下角的缩放抓手（QSizeGrip 在 resizeEvent 里重新定位）
    new QSizeGrip(root);

    connect(m_sideList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        const QString id = m_sideList->item(row)->data(Qt::UserRole).toString();
        if (!id.isEmpty())
            showTool(id);
    });

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        applySidebarFilter(text);
    });
}

void MainWindow::refreshSidebar()
{
    // QSignalBlocker：clear()/addItem 会触发 currentRowChanged，
    // 不挡住就会在刷新过程中顺手切换工具
    const QSignalBlocker blocker(m_sideList);
    m_sideList->clear();

    for (const ToolEntry &entry : m_registry->entries()) {
        auto *item = new QListWidgetItem(entry.icon, entry.name(), m_sideList);
        item->setData(Qt::UserRole, entry.info.id);
        item->setToolTip(entry.info.description);
    }

    const int n = m_registry->count();
    const int failed = m_plugins ? m_plugins->errors().size() : 0;
    m_footer->setText(failed > 0
                          ? QStringLiteral("已加载 %1 个工具 · %2 个失败").arg(n).arg(failed)
                          : QStringLiteral("已加载 %1 个工具").arg(n));
}

void MainWindow::applySidebarFilter(const QString &text)
{
    const QString q = text.trimmed();
    if (q.isEmpty()) {
        for (int i = 0; i < m_sideList->count(); ++i)
            m_sideList->item(i)->setHidden(false);
        return;
    }

    // 复用 ToolRegistry 的相关度排序取命中集，但只做显隐、保留侧边栏原始顺序：
    // 过滤时列表项位置还是稳定的，用户的肌肉记忆不会被打乱。
    const QStringList hits = m_registry->search(q, 1000);
    QSet<QString> hitSet;
    hitSet.reserve(hits.size());
    for (const QString &id : hits)
        hitSet.insert(id);

    for (int i = 0; i < m_sideList->count(); ++i) {
        const QString id = m_sideList->item(i)->data(Qt::UserRole).toString();
        m_sideList->item(i)->setHidden(!hitSet.contains(id));
    }
}

void MainWindow::updateHeader(const QString &title, const QString &desc)
{
    m_toolTitle->setText(title);
    m_toolDesc->setText(desc);
}

QWidget *MainWindow::panelFor(const QString &toolId)
{
    // 命中缓存就直接返回：面板里的输入状态因此能跨切换保留
    const auto it = m_pages.constFind(toolId);
    if (it != m_pages.constEnd())
        return m_stack->widget(it.value());

    const ToolEntry *entry = m_registry->entryById(toolId);
    if (!entry || !entry->tool)
        return nullptr;

    QWidget *panel = entry->tool->createPanel(nullptr);
    if (!panel) {
        // 插件违约（返回空）时不要崩，给个占位页
        auto *fallback = new QLabel(QStringLiteral("这个工具没有返回面板"), m_stack);
        fallback->setAlignment(Qt::AlignCenter);
        panel = fallback;
    }

    panel->setParent(m_stack);
    const int index = m_stack->addWidget(panel);
    m_pages.insert(toolId, index);

    // 面板被外部销毁时同步清掉索引，避免 m_pages 里留下失效指针
    connect(panel, &QObject::destroyed, this, [this, toolId] { m_pages.remove(toolId); });
    return panel;
}

void MainWindow::showTool(const QString &toolId, bool countUsage)
{
    const ToolEntry *entry = m_registry->entryById(toolId);
    if (!entry)
        return;

    QWidget *panel = panelFor(toolId);
    if (!panel)
        return;

    const int index = m_pages.value(toolId, -1);
    if (index < 0)
        return;

    m_stack->setCurrentIndex(index);
    updateHeader(entry->info.name, entry->info.description);

    // 同步侧边栏选中态（但不触发 currentRowChanged，否则会绕回来再调一次 showTool）
    for (int i = 0; i < m_sideList->count(); ++i) {
        if (m_sideList->item(i)->data(Qt::UserRole).toString() == toolId) {
            if (m_sideList->currentRow() != i) {
                const QSignalBlocker blocker(m_sideList);
                m_sideList->setCurrentRow(i);
            }
            break;
        }
    }

    m_settings->setLastToolId(toolId);
    if (countUsage)
        m_settings->bumpUsage(toolId);
}

void MainWindow::showSettings()
{
    m_stack->setCurrentWidget(m_settingsPage);
    updateHeader(QStringLiteral("设置"), QStringLiteral("热键、开机自启、插件目录与关于"));

    // 设置页不属于任何工具，清掉侧边栏选中态避免"高亮着某个工具却在看设置"
    const QSignalBlocker blocker(m_sideList);
    m_sideList->setCurrentRow(-1);
    m_sideList->clearSelection();
}

void MainWindow::focusSearch()
{
    show();
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();

    if (m_searchEdit) {
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();   // 全选：直接输入即替换旧关键词
    }
}

void MainWindow::selectLastTool()
{
    if (m_registry->count() == 0) {
        m_stack->setCurrentWidget(m_emptyPage);
        updateHeader(QStringLiteral("欢迎"), QStringLiteral("当前没有任何工具"));
        return;
    }

    const QString last = m_settings->lastToolId();
    if (!last.isEmpty() && m_registry->entryById(last)) {
        showTool(last, false);   // 恢复现场不计频次
        return;
    }
    showTool(m_registry->entries().first().info.id, false);
}

void MainWindow::prepareQuit()
{
    m_forceClose = true;
}

void MainWindow::setHotkeyStatus(const QString &text, bool ok)
{
    if (m_settingsPage)
        m_settingsPage->setHotkeyStatus(text, ok);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_forceClose) {
        event->accept();
        return;
    }
    // 关闭 = 收进托盘。配合 setQuitOnLastWindowClosed(false)，进程继续驻留。
    event->ignore();
    hide();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // 标题栏拖动：交给系统窗口管理器，比自己算 dx/dy 更跟手（也支持贴边吸附）
    if (watched == m_titleBar && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            if (QWindow *handle = windowHandle())
                handle->startSystemMove();
            return true;
        }
    }

    // 搜索框的键盘行为：回车打开第一个可见结果，上下在可见项之间移动，Esc 清空
    if (watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            for (int i = 0; i < m_sideList->count(); ++i) {
                if (!m_sideList->item(i)->isHidden()) {
                    m_sideList->setCurrentRow(i);
                    return true;
                }
            }
            return true;   // 没有结果就吞掉，避免触发按钮默认动作
        }
        case Qt::Key_Down:
            moveSideSelection(1);
            return true;
        case Qt::Key_Up:
            moveSideSelection(-1);
            return true;
        case Qt::Key_Escape:
            m_searchEdit->clear();
            return true;
        default:
            break;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::moveSideSelection(int delta)
{
    // 在可见项之间跳着移动：被过滤隐藏的项要跳过，否则按一下没反应
    const int n = m_sideList->count();
    if (n == 0)
        return;

    int target = m_sideList->currentRow() + delta;
    while (target >= 0 && target < n) {
        if (!m_sideList->item(target)->isHidden())
            break;
        target += delta;
    }
    if (target < 0 || target >= n)
        return;

    m_sideList->setCurrentRow(target);
    m_sideList->scrollToItem(m_sideList->item(target));
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    // QSizeGrip 不会自己跟着右下角走（它按布局定位，而这里是绝对定位），
    // 每次尺寸变化手动贴到右下角。
    if (auto *grip = findChild<QSizeGrip *>()) {
        grip->resize(16, 16);
        grip->move(width() - 18, height() - 18);
        grip->raise();
    }
}

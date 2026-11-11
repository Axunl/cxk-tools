#include "MainWindow.h"

#include "HomePage.h"
#include "IconBadge.h"
#include "LocalAppIndex.h"
#include "PluginManager.h"
#include "Settings.h"
#include "SettingsPage.h"
#include "ToolRegistry.h"
#include "ToolWindow.h"
#include "WindowGeometry.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizeGrip>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

namespace {

// 布局常量集中在这里：改一处即可，避免魔法数字散落
constexpr int kTitleHeight = 46;
constexpr int kPageMargin = 28;   ///< 页头与首页网格共用的左右留白，换页时标题不会跳
constexpr int kDefaultWidth = 1120;
constexpr int kDefaultHeight = 720;

/// 标题栏「两下」允许的位置误差（像素）。手抖几个像素仍算同一处；
/// 挪动明显了就不算 —— 否则「拖一下窗口，再点一下标题栏」会被误判成双击。
constexpr int kTitleDblClickSlop = 8;

} // namespace

MainWindow::MainWindow(ToolRegistry *registry, Settings *settings, PluginManager *plugins,
                       LocalAppIndex *apps, QWidget *parent)
    : QMainWindow(parent)
    , m_registry(registry)
    , m_settings(settings)
    , m_plugins(plugins)
    , m_apps(apps)
{
    // 无边框 + 自绘标题栏（与 uTools 那类工具面板观感一致）。
    // 拖动靠 QWindow::startSystemMove()，缩放靠 QSizeGrip，都是跨平台 API。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("CXK 工具箱"));
    setMinimumSize(920, 580);

    // 几何恢复要在 buildUi 之前：最小尺寸已设好，restore 才有意义
    WindowGeometry::restore(this, m_settings->windowGeometry(QStringLiteral("main")),
                            QSize(kDefaultWidth, kDefaultHeight));

    buildUi();
    showHome();   // 启动就停在首页网格
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

    // 设置入口：侧边栏取消后没有别的常驻入口了（只剩托盘菜单），补一个齿轮
    auto *settingsBtn = new QPushButton(QStringLiteral("\u2699"), m_titleBar);   // ⚙
    settingsBtn->setObjectName(QStringLiteral("titleBtnSettings"));
    settingsBtn->setFixedSize(30, 28);
    settingsBtn->setCursor(Qt::PointingHandCursor);
    settingsBtn->setToolTip(QStringLiteral("设置"));

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
    tb->addWidget(settingsBtn);
    tb->addWidget(minBtn);
    tb->addWidget(closeBtn);

    connect(settingsBtn, &QPushButton::clicked, this, &MainWindow::showSettings);
    connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);   // closeEvent 会改成 hide

    rootLayout->addWidget(m_titleBar);

    // ---------------------------------------------------------------- 页头 + 栈
    auto *column = new QWidget(root);
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(0);

    m_pageHeader = new QWidget(column);
    m_pageHeader->setObjectName(QStringLiteral("pageHeader"));
    auto *headerLayout = new QHBoxLayout(m_pageHeader);
    headerLayout->setContentsMargins(kPageMargin, 14, kPageMargin, 10);
    headerLayout->setSpacing(12);

    m_pageBack = new QPushButton(QStringLiteral("\u2190  首页"), m_pageHeader);
    m_pageBack->setObjectName(QStringLiteral("pageBack"));
    m_pageBack->setCursor(Qt::PointingHandCursor);

    auto *titleBox = new QVBoxLayout;
    titleBox->setContentsMargins(0, 0, 0, 0);
    titleBox->setSpacing(2);
    m_toolTitle = new QLabel(m_pageHeader);
    m_toolTitle->setObjectName(QStringLiteral("toolTitle"));
    m_toolDesc = new QLabel(m_pageHeader);
    m_toolDesc->setObjectName(QStringLiteral("toolDesc"));
    titleBox->addWidget(m_toolTitle);
    titleBox->addWidget(m_toolDesc);

    m_popoutBtn = new QPushButton(QStringLiteral("独立窗口"), m_pageHeader);
    m_popoutBtn->setObjectName(QStringLiteral("pagePopout"));
    m_popoutBtn->setCursor(Qt::PointingHandCursor);
    m_popoutBtn->setToolTip(QStringLiteral("把当前内容连同工具一起搬到独立窗口（双击标题栏也可以）"));

    headerLayout->addWidget(m_pageBack);
    headerLayout->addLayout(titleBox);
    headerLayout->addStretch(1);
    headerLayout->addWidget(m_popoutBtn);

    m_stack = new QStackedWidget(column);
    m_stack->setObjectName(QStringLiteral("toolStack"));

    m_home = new HomePage(m_registry, m_settings, m_plugins, m_apps, m_stack);
    m_stack->addWidget(m_home);

    m_settingsPage = new SettingsPage(m_settings, m_plugins, m_stack);
    m_stack->addWidget(m_settingsPage);

    columnLayout->addWidget(m_pageHeader);
    columnLayout->addWidget(m_stack, 1);
    rootLayout->addWidget(column, 1);

    // 无边框窗口右下角的缩放抓手（QSizeGrip 在 resizeEvent 里重新定位）
    new QSizeGrip(root);

    connect(m_pageBack, &QPushButton::clicked, this, &MainWindow::showHome);
    connect(m_popoutBtn, &QPushButton::clicked, this, [this] { popOutTool(m_currentToolId); });

    connect(m_home, &HomePage::toolActivated, this, [this](const QString &id) { showTool(id); });
    // 建议条打开工具时会带内容（JSON 原文 / 粘贴的图片 / 待翻译英文）
    connect(m_home, &HomePage::toolActivatedWithInput, this,
            [this](const QString &id, const xtools::ToolInput &input) { showTool(id, true, input); });
    connect(m_home, &HomePage::toolPopOutRequested, this, &MainWindow::popOutTool);
    connect(m_home, &HomePage::appActivated, this, &MainWindow::launchLocalApp);

    connect(m_settingsPage, &SettingsPage::restartRequested, this, &MainWindow::restartRequested);
    // 更新请求原样上抛：界面层不持有 UpdateChecker，避免 ui 反向依赖 core 的网络实现细节
    connect(m_settingsPage, &SettingsPage::checkUpdateRequested, this, &MainWindow::checkUpdateRequested);
    connect(m_settingsPage, &SettingsPage::hotkeyChanged, this, [this](const QKeySequence &seq) {
        // 标题栏那行提示要跟着变，否则用户改完热键看到的还是旧组合
        if (m_hotkeyHint)
            m_hotkeyHint->setText(seq.toString(QKeySequence::NativeText));
        emit hotkeyChanged(seq);
    });
}

void MainWindow::createToolPanel(const QString &toolId, const xtools::ToolInput &input)
{
    // 面板不缓存：每次进入工具都新建，输入过的值不留存。
    // ITool::createPanel 的接口契约本就要求"每次返回全新面板"，插件无需改动。
    const ToolEntry *entry = m_registry->entryById(toolId);
    if (!entry || !entry->tool)
        return;

    QWidget *panel = entry->tool->createPanel(nullptr);
    if (!panel) {
        // 插件违约（返回空）时不要崩，给个占位页
        auto *fallback = new QLabel(QStringLiteral("这个工具没有返回面板"), m_stack);
        fallback->setAlignment(Qt::AlignCenter);
        panel = fallback;
    }

    panel->setParent(m_stack);
    m_stack->addWidget(panel);
    m_toolPanel = panel;

    // 面板已创建、尚未显示：此时把首页带进来的内容交给插件正合适
    // （面板可以在 showEvent 里按已有内容排版/聚焦，用户看不到中间态）。
    if (input.kind == xtools::ToolInput::Kind::None)
        return;

    // ITool 不是 QObject，但插件对象一定同时继承 QObject（QPluginLoader 只认 QObject），
    // 先转成 QObject* 才能用 qobject_cast 查它有没有实现可选接口 IToolInput。
    auto *object = dynamic_cast<QObject *>(entry->tool);
    auto *receiver = object ? qobject_cast<xtools::IToolInput *>(object) : nullptr;
    if (!receiver) {
        // 旧插件不实现这个接口是合法状态：只打开工具、不带内容，不要当错误处理
        qInfo() << "MainWindow: 工具不接受带入内容" << toolId;
        return;
    }
    receiver->setToolInput(input);
}

void MainWindow::destroyToolPanel()
{
    if (!m_toolPanel)
        return;
    m_stack->removeWidget(m_toolPanel);
    m_toolPanel->deleteLater();
    m_toolPanel = nullptr;
}

void MainWindow::showHome()
{
    destroyToolPanel();
    m_currentToolId.clear();

    // 页头整块隐藏：首页要拿到完整的内容区高度（uTools 观感的关键）
    m_pageHeader->hide();
    m_stack->setCurrentWidget(m_home);
    m_home->focusSearch();
}

void MainWindow::showTool(const QString &toolId, bool countUsage, const xtools::ToolInput &input)
{
    const ToolEntry *entry = m_registry->entryById(toolId);
    if (!entry)
        return;

    // 离开当前工具视图即销毁面板；新工具一律全新面板
    destroyToolPanel();
    createToolPanel(toolId, input);
    if (!m_toolPanel)
        return;

    m_stack->setCurrentWidget(m_toolPanel);
    m_pageHeader->show();
    m_popoutBtn->show();
    updateHeader(entry->info.name, entry->info.description);
    m_currentToolId = toolId;

    if (countUsage)
        markUsed(toolId);
}

void MainWindow::showSettings()
{
    // 离开工具视图即销毁面板：设置页不是工具，回到工具时会重新创建空面板
    destroyToolPanel();
    m_currentToolId.clear();

    m_stack->setCurrentWidget(m_settingsPage);
    m_pageHeader->show();
    m_popoutBtn->hide();   // 设置页没有「独立窗口」语义
    updateHeader(QStringLiteral("设置"), QStringLiteral("热键、开机自启、插件目录与关于"));
}

QWidget *MainWindow::takeCurrentToolPanel(const QString &toolId)
{
    // 只有「当前显示的就是这个工具」才谈得上移交；否则返回 nullptr 让调用方新建面板
    if (!m_toolPanel || m_currentToolId != toolId)
        return nullptr;

    QWidget *panel = m_toolPanel;

    // 先把成员清空再动窗口：showHome()/closeEvent 都会走 destroyToolPanel()，
    // 那时 m_toolPanel 已经是 nullptr，不会把要移交的面板删掉。
    m_toolPanel = nullptr;
    m_currentToolId.clear();
    m_stack->removeWidget(panel);   // 从栈里摘掉，但**不 delete**：面板要搬去独立窗口

    // 主窗口回首页：工具已经被搬走了，留一张空壳工具页既没有内容也没有意义。
    // 用户还想在主窗口里用同一个工具，从网格再点一次即可（会得到全新面板）。
    showHome();
    return panel;
}

void MainWindow::popOutCurrentTool()
{
    // 首页/设置页没有"当前工具"，静默忽略：标题栏双击在那些页面上本就该什么都不做
    if (m_currentToolId.isEmpty())
        return;
    popOutTool(m_currentToolId);
}

void MainWindow::popOutTool(const QString &toolId)
{
    if (toolId.isEmpty())
        return;
    const ToolEntry *entry = m_registry->entryById(toolId);
    if (!entry)
        return;

    // QPointer 已销毁时隐式转成 nullptr，所以「窗口还开着」和「用户已关掉」共用一条判断
    if (ToolWindow *existing = m_toolWindows.value(toolId)) {
        // 已经有独立窗口：只前台化。此时内嵌面板原样留在主窗口里 —— 不动它的数据，
        // 用户手上那份输入不会被另一份覆盖掉。
        existing->showAndRaise();
        markUsed(toolId);
        return;
    }

    // 关键：把内嵌面板整块交给独立窗口（同一个 QWidget 实例），而不是重新 createPanel()。
    // 这样正在编辑的内容、光标、撤销栈、滚动位置、状态文案全部原样过去，插件零改动。
    QWidget *adopted = takeCurrentToolPanel(toolId);

    auto *window = new ToolWindow(*entry, adopted, nullptr);
    // WA_DeleteOnClose 会先析构窗口再发 destroyed，这里顺手把表项摘掉
    connect(window, &QObject::destroyed, this, [this, toolId] { m_toolWindows.remove(toolId); });
    m_toolWindows.insert(toolId, window);
    window->showAndRaise();

    markUsed(toolId);
}

void MainWindow::closeAllToolWindows()
{
    // 先拷贝一份再关：close() 会触发 destroyed -> 从哈希里移除，
    // 直接迭代哈希会在中途被改而失效。
    const auto windows = m_toolWindows.values();
    for (const QPointer<ToolWindow> &window : windows) {
        if (window)
            window->close();
    }
    m_toolWindows.clear();
}

void MainWindow::saveWindowState()
{
    m_settings->setWindowGeometry(QStringLiteral("main"), saveGeometry());
}

void MainWindow::focusSearch()
{
    show();
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();

    // 呼出即回到首页（uTools 语义）：不再恢复上次的工具页。
    // 想保留正在编辑的内容，用独立窗口打开。
    showHome();
}

void MainWindow::markUsed(const QString &toolId)
{
    // 只有用户主动打开才记账：启动/唤出恢复不计，否则频次与最近使用会被启动次数污染
    m_settings->bumpUsage(toolId);
    m_settings->touchRecent(toolId);
}

void MainWindow::launchLocalApp(const QString &appId)
{
    if (!m_apps)
        return;

    QString error;
    if (!m_apps->launch(appId, &error)) {
        // 入口被删/权限不足时给出可读原因，而不是静默失败让用户以为点错了
        QMessageBox::warning(this, QStringLiteral("打不开这个应用"), error);
        return;
    }

    // 启动器语义：把用户送出去之后自己收起，不留在前面挡着（uTools 同样处理）。
    // 失败不记账；成功后写入的是同一个「最近使用」序列，工具与应用一起排序。
    m_settings->bumpUsage(appId);
    m_settings->touchRecent(appId);
    close();
}

void MainWindow::searchFor(const QString &query)
{
    showHome();
    m_home->setSearchText(query);
}

void MainWindow::updateHeader(const QString &title, const QString &desc)
{
    m_toolTitle->setText(title);
    m_toolDesc->setText(desc);
}

void MainWindow::prepareQuit()
{
    m_forceClose = true;

    // 退出是唯一可靠的清理点：qApp->quit() 不会给窗口发 closeEvent，
    // 所以独立窗口必须在这里关，几何也在这里落盘。
    saveWindowState();
    closeAllToolWindows();
    m_settings->sync();
}

void MainWindow::setHotkeyStatus(const QString &text, bool ok)
{
    if (m_settingsPage)
        m_settingsPage->setHotkeyStatus(text, ok);
}

void MainWindow::setUpdateStatus(const QString &text, bool ok)
{
    if (m_settingsPage)
        m_settingsPage->setUpdateStatus(text, ok);
}

void MainWindow::setUpdateChecking(bool checking)
{
    if (m_settingsPage)
        m_settingsPage->setUpdateChecking(checking);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_forceClose) {
        event->accept();
        return;
    }
    // 关闭 = 收进托盘。配合 setQuitOnLastWindowClosed(false)，进程继续驻留。
    // 收起窗口也销毁内嵌工具面板（无缓存，输入值不留存）；独立窗口不跟着关。
    destroyToolPanel();
    m_currentToolId.clear();
    saveWindowState();
    event->ignore();
    hide();
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    // Esc 分层（只认裸 Esc，带修饰键的组合留给子控件/系统）：
    //   搜索框有字 → 由 HomePage 的 eventFilter 先清空并吞掉，走不到这里；
    //   工具页 / 设置页 → 回首页（可逆的一步，比直接收托盘更符合直觉）；
    //   首页 → 收进托盘，与标题栏 ✕ 等价。
    //
    // 走按键传播而不是 QShortcut：子控件先拿到按键，谁用谁吞掉，天然分优先级。
    // 例如设置页热键捕获中的 Esc 语义是"取消捕获"，HotkeyButton::keyPressEvent
    // 接受了该事件，根本不会传到这里，不会误回首页。
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        event->accept();
        if (m_stack->currentWidget() != m_home)
            showHome();
        else
            close();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_titleBar) {
        switch (event->type()) {
        case QEvent::MouseButtonDblClick: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_titleClickValid = false;   // 清掉计时：三连击不要再多弹一次
                popOutCurrentTool();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() != Qt::LeftButton)
                break;

            const QPoint pos = me->position().toPoint();

            // 自己再判一次「两下」：第一次按下已经把拖动交给了系统移动循环
            // （startSystemMove 在 Windows 上会进原生移动流程，鼠标被系统接管），
            // 第二次按下有可能只以普通 Press 到达，凑不成 Qt 的 DblClick。
            // 两条路都走同一个动作，就算都触发也只是前台化同一个窗口，不会重复开窗。
            const bool secondClick = m_titleClickValid
                && m_titleClickTimer.elapsed() <= QApplication::doubleClickInterval()
                && (pos - m_titleClickPos).manhattanLength() <= kTitleDblClickSlop;

            m_titleClickValid = false;
            if (secondClick) {
                popOutCurrentTool();
                return true;
            }

            m_titleClickTimer.start();
            m_titleClickValid = true;
            m_titleClickPos = pos;

            // 标题栏拖动：交给系统窗口管理器，比自己算 dx/dy 更跟手（也支持贴边吸附）
            if (QWindow *handle = windowHandle())
                handle->startSystemMove();
            return true;
        }
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    // QSizeGrip 不会自己跟着右下角走（它按布局定位，而这里是绝对定位），
    // 每次尺寸变化手动贴到右下角。恢复窗口几何后也依赖这里重新摆。
    if (auto *grip = findChild<QSizeGrip *>()) {
        grip->resize(16, 16);
        grip->move(width() - 18, height() - 18);
        grip->raise();
    }
}

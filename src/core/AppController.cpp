#include "AppController.h"

#include "HotkeyManager.h"
#include "LocalAppIndex.h"
#include "PluginManager.h"
#include "Settings.h"
#include "ToolRegistry.h"
#include "UpdateChecker.h"
#include "ui/MainWindow.h"
#include "ui/TrayController.h"
#include "ui/UpdateDialog.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QProcess>
#include <QTimer>

AppController::AppController(QObject *parent)
    : QObject(parent)
{
}

AppController::~AppController() = default;

int AppController::runHeadlessCheck()
{
    // 用局部检查器而不是成员：这个分支不创建窗口，也不需要托盘/设置页的联动。
    UpdateChecker checker;
    QEventLoop loop;
    int code = 2;

    connect(&checker, &UpdateChecker::updateAvailable, &loop, [&code, &loop](const update::Info &info) {
        qInfo().noquote() << "CHECK_UPDATE_RESULT=update-available version=" << info.version
                          << "tag=" << info.tag << "assets=" << info.assets.size();
        code = 0;
        loop.quit();
    });
    connect(&checker, &UpdateChecker::upToDate, &loop, [&code, &loop](const QString &version) {
        qInfo().noquote() << "CHECK_UPDATE_RESULT=up-to-date version=" << version;
        code = 1;
        loop.quit();
    });
    connect(&checker, &UpdateChecker::failed, &loop, [&code, &loop](const QString &message) {
        qWarning().noquote() << "CHECK_UPDATE_RESULT=failed reason=" << message;
        code = 2;
        loop.quit();
    });

    checker.check();

    // 兜底超时：transferTimeout 之外再兜一层，保证脚本调用不会挂住
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);
    loop.exec();
    return code;
}

void AppController::start(bool autostartLaunch)
{
    // --check-update：无界面检查一次更新后退出。这是更新链路的可自动化验证入口：
    // 结果写进日志（CHECK_UPDATE_RESULT=…），配合 XTOOLS_UPDATE_FEED 可以完全离线跑。
    // 必须放在建窗口之前，否则会闪一下窗口，也会抢单实例锁。
    if (QCoreApplication::arguments().contains(QStringLiteral("--check-update"))) {
        const int code = runHeadlessCheck();
        qInfo() << "AppController: --check-update finished, code" << code;
        // 不能直接 qApp->quit()：此刻 exec() 还没开始跑，Qt 会忽略这次退出请求，
        // 进程就挂在那里等超时。排到事件循环的第一轮再退，并带上退出码给脚本用。
        QTimer::singleShot(0, qApp, [code] { QCoreApplication::exit(code); });
        return;
    }

    m_settings = &Settings::instance();
    m_registry = new ToolRegistry(this);
    m_plugins = new PluginManager(m_registry, this);
    m_apps = new LocalAppIndex(this);
    m_hotkey = new HotkeyManager(this);

    // 先加载插件再建窗口：MainWindow 构造时要按已注册的工具铺首页网格
    m_plugins->loadAll();

    // 本机应用索引：只遍历目录，图标按需读取，正常机器是毫秒级。
    // 运行期新装的应用不热更新，重启后才出现（与插件扫描的策略一致）。
    m_apps->scan();

    m_main = new MainWindow(m_registry, m_settings, m_plugins, m_apps);
    m_tray = new TrayController(this);
    m_update = new UpdateChecker(this);
    m_updateDialog = new UpdateDialog(m_update, m_settings, m_main);

    // ---- 接线：热键 / 托盘 -> 呼出窗口并聚焦搜索框
    connect(m_hotkey, &HotkeyManager::activated, this, [this] {
        m_main->focusSearch();
    });

    connect(m_tray, &TrayController::launcherRequested, this, [this] {
        m_main->focusSearch();
    });
    connect(m_tray, &TrayController::mainWindowRequested, this, &AppController::showMainWindow);
    connect(m_tray, &TrayController::settingsRequested, this, [this] {
        showMainWindow();
        m_main->showSettings();
    });
    connect(m_tray, &TrayController::restartRequested, this, &AppController::restart);
    connect(m_tray, &TrayController::quitRequested, this, &AppController::quit);
    connect(m_main, &MainWindow::restartRequested, this, &AppController::restart);

    // 设置页改热键 -> 立刻重新注册（失败会把原因回显到设置页）
    connect(m_main, &MainWindow::hotkeyChanged, this, [this](const QKeySequence &seq) {
        registerHotkey(seq);
    });

    // ---- 接线：更新
    // 三处触发（设置页按钮 / 托盘菜单 / 启动静默）都收敛到 checkUpdate()，
    // 只有它决定要不要把结果显示到界面上。
    connect(m_main, &MainWindow::checkUpdateRequested, this, [this] { checkUpdate(true); });
    connect(m_tray, &TrayController::checkUpdateRequested, this, [this] {
        // 从托盘进来的用户看不到设置页，先把设置页翻出来再检查，结果才有地方显示
        showMainWindow();
        m_main->showSettings();
        checkUpdate(true);
    });
    // 点「发现新版本」的气泡 -> 展开更新对话框（不再联网，用缓存下来的清单）
    connect(m_tray, &TrayController::messageClicked, this, [this] {
        if (m_hasPendingUpdate)
            showUpdateDialog();
    });

    connect(m_update, &UpdateChecker::checking, this, [this] {
        m_main->setUpdateChecking(true);
    });

    connect(m_update, &UpdateChecker::updateAvailable, this, [this](const update::Info &info) {
        m_pendingUpdate = info;
        m_hasPendingUpdate = true;
        m_main->setUpdateChecking(false);
        m_main->setUpdateStatus(QStringLiteral("发现新版本 %1").arg(info.version), true);

        if (m_silentCheck) {
            // 静默检查不抢焦点：只弹气泡，用户点了才展开对话框
            m_tray->showMessage(QStringLiteral("发现新版本 %1").arg(info.version),
                                QStringLiteral("点击查看更新说明并升级"),
                                false);
            qInfo() << "AppController: 发现新版本（静默提示）" << info.version;
        } else {
            showUpdateDialog();
        }
    });

    connect(m_update, &UpdateChecker::upToDate, this, [this](const QString &version) {
        m_main->setUpdateChecking(false);
        if (!m_silentCheck)
            m_main->setUpdateStatus(QStringLiteral("已是最新版本（%1）").arg(version), true);
        qInfo() << "AppController: 已是最新版本" << version;
    });

    connect(m_update, &UpdateChecker::failed, this, [this](const QString &message) {
        m_main->setUpdateChecking(false);
        // 静默检查失败不打扰用户（可能是没网），只留日志
        if (!m_silentCheck)
            m_main->setUpdateStatus(message, false);
        qWarning() << "AppController: 检查更新失败:" << message;
    });

    // 安装器已经跑起来：立刻退出，让安装器能覆盖 exe 与插件
    connect(m_updateDialog, &UpdateDialog::installerLaunched, this, &AppController::quit);

    registerHotkey(m_settings->hotkey());

    const QStringList args = QCoreApplication::arguments();

    // 目标页参数：--tool <id> 进内嵌工具页，--popout <id> 直接开独立窗口，
    // --search <关键词> 停在首页并预填搜索（可搜到本机应用）。
    // 只解析一次：下面截图分支与正常启动分支共用。
    auto valueAfter = [&args](const QString &flag) -> QString {
        const int index = args.indexOf(flag);
        if (index >= 0 && index + 1 < args.size())
            return args.at(index + 1);
        return QString();
    };
    const QString toolTarget = valueAfter(QStringLiteral("--tool"));
    const QString popoutTarget = valueAfter(QStringLiteral("--popout"));
    const QString searchTarget = valueAfter(QStringLiteral("--search"));

    // ---- 调试用：把窗口渲染成 PNG 后退出
    // 无头环境（CI/沙箱）截不了屏，这个开关是界面回归的唯一手段。
    // 注意顺序：showMainWindow() 会落到首页，切页动作必须排在它之后，
    // 否则截出来永远是首页。
    const int shotIndex = args.indexOf(QStringLiteral("--shot"));
    if (shotIndex >= 0 && shotIndex + 1 < args.size()) {
        const QString path = args.at(shotIndex + 1);

        showMainWindow();
        if (args.contains(QStringLiteral("--settings")))
            m_main->showSettings();
        else if (!toolTarget.isEmpty())
            m_main->showTool(toolTarget);
        else if (!popoutTarget.isEmpty())
            m_main->popOutTool(popoutTarget);
        else if (!searchTarget.isEmpty())
            m_main->searchFor(searchTarget);

        // 等一轮布局+样式应用完成再抓图，否则可能抓到未布局的空窗口
        QTimer::singleShot(500, this, [this, path, popoutTarget] {
            // --popout 要截独立窗口：它是最顶层窗口，不在 m_main 的子树里
            QWidget *target = m_main;
            if (!popoutTarget.isEmpty()) {
                const auto tops = QApplication::topLevelWidgets();
                for (QWidget *w : tops) {
                    if (w->objectName() == QLatin1String("toolWindow")) {
                        target = w;
                        break;
                    }
                }
            }
            const bool ok = target->grab().save(path);
            qInfo() << "AppController: screenshot" << (ok ? "saved" : "failed") << path;
            qApp->quit();
        });
        return;
    }

    // ---- 直接进设置页 / 直接进工具页（命令行、快捷方式）
    if (args.contains(QStringLiteral("--settings")) || !toolTarget.isEmpty()) {
        showMainWindow();
        if (!toolTarget.isEmpty())
            m_main->showTool(toolTarget);
        else
            m_main->showSettings();
        return;
    }

    if (!popoutTarget.isEmpty()) {
        m_main->popOutTool(popoutTarget);
        return;
    }

    // ---- 预填搜索（调试/脚本唤起；正常启动仍停在空搜索的首页）
    if (!searchTarget.isEmpty()) {
        showMainWindow();
        m_main->searchFor(searchTarget);
        return;
    }

    // ---- 开机自启：静默驻留托盘
    if (autostartLaunch) {
        qInfo() << "AppController: started silently (autostart) with" << m_registry->count() << "tools";
        // 自启时不弹窗，但仍然值得在后台看看有没有新版本（一次气泡比手动找发布页省事）
        m_silentCheck = true;
        m_update->scheduleStartupCheck();
        return;
    }

    // ---- 正常启动：直接打开主窗口，停在首页网格
    showMainWindow();

    if (m_registry->count() == 0) {
        // 一个工具都没有时给个引导，否则用户看到空窗口不知道下一步做什么
        m_tray->showMessage(QStringLiteral("CXK 工具箱"),
                            QStringLiteral("还没有加载到工具。把插件动态库放进 plugins 目录后重启即可。"),
                            true);
    }

    // 延迟 3 秒、24 小时一次；失败只写日志，不影响启动
    m_silentCheck = true;
    m_update->scheduleStartupCheck();

    qInfo() << "AppController: started with" << m_registry->count() << "tools,"
            << m_plugins->errors().size() << "plugin errors,"
            << "main window visible" << m_main->isVisible();
}

void AppController::registerHotkey(const QKeySequence &seq)
{
    QString error;
    if (m_hotkey->registerSequence(seq, &error)) {
        const QString text = seq.toString(QKeySequence::NativeText);
        m_tray->setHotkeyText(text);
        if (m_main)
            m_main->setHotkeyStatus(QStringLiteral("已生效"), true);
        qInfo() << "AppController: hotkey ready:" << text;
        return;
    }

    // 失败不阻止启动：用户在设置里换一个组合键即可
    qWarning() << "AppController: hotkey registration failed:" << error;
    m_tray->setHotkeyText(QStringLiteral("未注册"));
    if (m_main)
        m_main->setHotkeyStatus(error, false);
    m_tray->showMessage(QStringLiteral("热键注册失败"),
                        error + QStringLiteral("，请在设置里换一个组合键"),
                        true);
}

void AppController::showMainWindow()
{
    // 统一落首页：主窗口每次被"打开"都回到网格选择器（热键/托盘/二次激活一致）
    m_main->showHome();
    m_main->show();
    if (m_main->isMinimized())
        m_main->showNormal();
    m_main->raise();
    m_main->activateWindow();
}

void AppController::quit()
{
    // 必须先解除"关闭即收进托盘"，否则 quit() 会被 closeEvent 拦下来
    if (m_main)
        m_main->prepareQuit();
    qApp->quit();
}

void AppController::checkUpdate(bool fromUser)
{
    // 静默与手动的区别只体现在「结果怎么显示」（气泡 vs 设置页 + 对话框），
    // 网络行为完全一致，所以这里只记一个标记。
    m_silentCheck = !fromUser;
    if (fromUser) {
        m_main->setUpdateChecking(true);
        qInfo() << "AppController: 手动检查更新";
    }
    m_update->check();
}

void AppController::showUpdateDialog()
{
    if (!m_hasPendingUpdate)
        return;

    // 弹窗前把主窗口收回到可见状态：对话框是 m_main 的子窗口，主窗口在托盘里时
    // 只显示子窗口会让用户找不到它。
    if (m_main && !m_main->isVisible())
        showMainWindow();
    m_updateDialog->showUpdate(m_pendingUpdate);
}

void AppController::restart()
{
    const QString exe = QCoreApplication::applicationFilePath();
    qInfo() << "AppController: restarting" << exe;

    if (m_main)
        m_main->prepareQuit();

    // 先起新进程再退自己：反过来的话新进程会因为单实例互斥直接退出
    const bool ok = QProcess::startDetached(exe, QStringList{});
    if (!ok) {
        qWarning() << "AppController: restart failed";
        return;
    }
    qApp->quit();
}

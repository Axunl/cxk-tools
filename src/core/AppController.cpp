#include "AppController.h"

#include "HotkeyManager.h"
#include "PluginManager.h"
#include "Settings.h"
#include "ToolRegistry.h"
#include "ui/MainWindow.h"
#include "ui/TrayController.h"

#include <QApplication>
#include <QDebug>
#include <QProcess>
#include <QTimer>

AppController::AppController(QObject *parent)
    : QObject(parent)
{
}

AppController::~AppController() = default;

void AppController::start(bool autostartLaunch)
{
    m_settings = &Settings::instance();
    m_registry = new ToolRegistry(this);
    m_plugins = new PluginManager(m_registry, this);
    m_hotkey = new HotkeyManager(this);

    // 先加载插件再建窗口：MainWindow 构造时要按已注册的工具填侧边栏
    m_plugins->loadAll();

    m_main = new MainWindow(m_registry, m_settings, m_plugins);
    m_tray = new TrayController(this);

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

    registerHotkey(m_settings->hotkey());

    // 恢复上次工具；此时不应累计使用频次，否则频率会被启动次数污染
    m_main->selectLastTool();

    const QStringList args = QCoreApplication::arguments();

    // ---- 调试用：把窗口渲染成 PNG 后退出
    // 无头环境（CI/沙箱）截不了屏，这个开关是界面回归的唯一手段。
    const int shotIndex = args.indexOf(QStringLiteral("--shot"));
    if (shotIndex >= 0 && shotIndex + 1 < args.size()) {
        const QString path = args.at(shotIndex + 1);

        if (args.contains(QStringLiteral("--settings")))
            m_main->showSettings();
        showMainWindow();

        // 等一轮布局+样式应用完成再抓图，否则可能抓到未布局的空窗口
        QTimer::singleShot(500, this, [this, path] {
            const bool ok = m_main->grab().save(path);
            qInfo() << "AppController: screenshot" << (ok ? "saved" : "failed") << path;
            qApp->quit();
        });
        return;
    }

    // ---- 直接进设置页（命令行/快捷方式）
    if (args.contains(QStringLiteral("--settings"))) {
        showMainWindow();
        m_main->showSettings();
        return;
    }

    // ---- 开机自启：静默驻留托盘
    if (autostartLaunch) {
        qInfo() << "AppController: started silently (autostart) with" << m_registry->count() << "tools";
        return;
    }

    // ---- 正常启动：直接打开主窗口，停在上次使用的工具
    showMainWindow();

    if (m_registry->count() == 0) {
        // 一个工具都没有时给个引导，否则用户看到空窗口不知道下一步做什么
        m_tray->showMessage(QStringLiteral("CXK 工具箱"),
                            QStringLiteral("还没有加载到工具。把插件动态库放进 plugins 目录后重启即可。"),
                            true);
    }

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

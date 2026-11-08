#pragma once

// ---------------------------------------------------------------------------
// 组装层：把配置、注册表、插件、热键、窗口、托盘拼起来并接线。
//
// 为什么集中在这里：其他类都不持有彼此的引用，只暴露信号/方法；所有对象
// 的创建顺序与信号连接只在这一个文件里，排查"谁调用了谁"时不用满仓库找。
// ---------------------------------------------------------------------------

#include "UpdateInfo.h"

#include <QKeySequence>
#include <QObject>
#include <QString>

class Settings;
class ToolRegistry;
class PluginManager;
class LocalAppIndex;
class HotkeyManager;
class MainWindow;
class TrayController;
class UpdateChecker;
class UpdateDialog;

class AppController : public QObject
{
    Q_OBJECT

public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    /// 启动流程入口。
    /// @param autostartLaunch 由 --autostart 触发时为 true：只驻留托盘，不弹窗口
    void start(bool autostartLaunch);

    /// 显示主窗口（从最小化/托盘恢复并置前）。单实例激活、托盘菜单都走这里。
    void showMainWindow();

private:
    void registerHotkey(const QKeySequence &seq);
    void quit();
    void restart();

    /// 无界面地跑一次更新检查，结果写日志后返回（--check-update）。
    /// 返回 0=有新版本 1=已是最新 2=失败，仅用于日志可读性。
    int runHeadlessCheck();
    /// fromUser=true 时把过程与结果回显到设置页；false 时只留日志与托盘气泡。
    void checkUpdate(bool fromUser);
    void showUpdateDialog();

    Settings *m_settings = nullptr;
    ToolRegistry *m_registry = nullptr;
    PluginManager *m_plugins = nullptr;
    LocalAppIndex *m_apps = nullptr;
    HotkeyManager *m_hotkey = nullptr;
    MainWindow *m_main = nullptr;
    TrayController *m_tray = nullptr;
    UpdateChecker *m_update = nullptr;
    UpdateDialog *m_updateDialog = nullptr;

    /// 静默检查发现的新版本：点托盘气泡时用它弹窗，避免再联网一次。
    update::Info m_pendingUpdate;
    bool m_hasPendingUpdate = false;
    /// 当前这次检查是不是静默触发的（决定「发现新版本」时弹窗还是只弹气泡）。
    bool m_silentCheck = false;
};

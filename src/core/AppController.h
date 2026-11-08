#pragma once

// ---------------------------------------------------------------------------
// 组装层：把配置、注册表、插件、热键、窗口、托盘拼起来并接线。
//
// 为什么集中在这里：其他类都不持有彼此的引用，只暴露信号/方法；所有对象
// 的创建顺序与信号连接只在这一个文件里，排查"谁调用了谁"时不用满仓库找。
// ---------------------------------------------------------------------------

#include <QKeySequence>
#include <QObject>
#include <QString>

class Settings;
class ToolRegistry;
class PluginManager;
class HotkeyManager;
class MainWindow;
class TrayController;

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

    Settings *m_settings = nullptr;
    ToolRegistry *m_registry = nullptr;
    PluginManager *m_plugins = nullptr;
    HotkeyManager *m_hotkey = nullptr;
    MainWindow *m_main = nullptr;
    TrayController *m_tray = nullptr;
};

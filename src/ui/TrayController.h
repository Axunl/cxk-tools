#pragma once

// ---------------------------------------------------------------------------
// 托盘：常驻图标 + 右键菜单。
//
// 主窗口关闭后应用靠它"活着"，所以菜单项必须覆盖用户可能想做的全部操作：
// 呼出搜索、打开主窗口、设置、重新扫描（重启）、退出。
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;

class TrayController : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(QObject *parent = nullptr);

    /// 把当前热键写进菜单文案（"呼出搜索（Ctrl+Alt+Space）"）。
    /// 注册失败时传"未注册"，让用户一眼看到不是快捷键的问题。
    void setHotkeyText(const QString &text);

    /// 弹气泡提示（插件全挂、热键冲突等）。
    /// @param warning 用警告图标而不是信息图标
    void showMessage(const QString &title, const QString &body, bool warning = false);

signals:
    void launcherRequested();
    void mainWindowRequested();
    void settingsRequested();
    void checkUpdateRequested();
    void restartRequested();
    void quitRequested();
    /// 用户点了气泡提示（目前用于「发现新版本 → 打开更新对话框」）。
    void messageClicked();

private:
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_launcherAction = nullptr;
};

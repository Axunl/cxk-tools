#pragma once

// ---------------------------------------------------------------------------
// 主窗口：无边框自绘标题栏 + 「首页网格 / 设置页 / 工具页」三层栈。
//
// 关键设计：
//   * 首页是插件图标网格（HomePage）：启动、热键/托盘唤出、Esc 从工具页返回都停在首页。
//     不再有「左侧列表 + 右侧实现」的并排布局。
//   * 工具面板**不缓存**：每次打开工具都 createPanel() 新建面板；离开工具视图
//     （回首页、进设置、收起窗口）即销毁面板，输入值随之清空。
//   * 本机应用只索引、不嵌入：点应用磁贴交给系统启动，然后把自己收进托盘
//     （启动器语义，跟 uTools 一致）。应用与工具共用「最近使用」序列。
//   * 每个工具最多一个独立窗口（ToolWindow），重复打开只前台化已有窗口。
//     独立窗口主窗口收起时继续存活，退出时统一关闭（关闭动作在 prepareQuit()，
//     因为 qApp->quit() 不会给窗口发 closeEvent）。
//   * 关闭按钮与 Esc 语义：首页 → 收进托盘；工具页/设置页 → 先回首页。
//     真正退出必须先 prepareQuit()，否则会被 closeEvent 拦下。
// ---------------------------------------------------------------------------

#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QPointer>
#include <QString>

class HomePage;
class LocalAppIndex;
class PluginManager;
class Settings;
class SettingsPage;
class ToolRegistry;
class ToolWindow;
class QLabel;
class QPushButton;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(ToolRegistry *registry, Settings *settings, PluginManager *plugins,
               LocalAppIndex *apps, QWidget *parent = nullptr);

    /// 回到首页网格：销毁内嵌工具面板、隐藏页头、聚焦搜索框。
    /// 启动、热键/托盘唤出、Esc 从工具页返回都走这里。
    void showHome();

    /// 在主窗口内打开指定工具。
    /// @param countUsage 只有用户主动打开才累计频次与最近使用；程序内恢复传 false。
    void showTool(const QString &toolId, bool countUsage = true);

    /// 把工具弹成独立窗口。已有该工具的窗口时不新建，只前台化。
    void popOutTool(const QString &toolId);

    void showSettings();

    /// 真正的退出：允许窗口关闭（否则 close() 只是收进托盘）。
    /// 顺带存窗口几何、关掉所有独立窗口 —— 这是唯一可靠的退出清理点。
    void prepareQuit();

    /// 呼出：窗口前台显示并回到首页、把焦点落到搜索框（热键/托盘"呼出搜索"都走这里）。
    void focusSearch();

    /// 回到首页并填入关键词（命令行 --search，也用于界面回归截图）。
    void searchFor(const QString &query);

    /// 设置页的热键状态回显（"已生效" / 失败原因）。
    void setHotkeyStatus(const QString &text, bool ok);

signals:
    void restartRequested();
    void hotkeyChanged(const QKeySequence &seq);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void createToolPanel(const QString &toolId);
    void destroyToolPanel();
    void updateHeader(const QString &title, const QString &desc);
    /// 记录一次「用户主动打开了这个工具」：频次 + 首页的最近使用。
    void markUsed(const QString &toolId);
    /// 启动本机应用；成功后同样记一笔频次/最近使用，然后收起主窗口。
    void launchLocalApp(const QString &appId);
    void closeAllToolWindows();
    void saveWindowState();

    ToolRegistry *m_registry = nullptr;
    Settings *m_settings = nullptr;
    PluginManager *m_plugins = nullptr;
    LocalAppIndex *m_apps = nullptr;

    QWidget *m_titleBar = nullptr;
    QLabel *m_hotkeyHint = nullptr;   ///< 标题栏上的热键提示，改热键时要同步刷新

    QWidget *m_pageHeader = nullptr;   ///< 工具页/设置页的页头，首页时整块隐藏
    QPushButton *m_pageBack = nullptr;
    QPushButton *m_popoutBtn = nullptr;   ///< 「独立窗口」，设置页时隐藏
    QLabel *m_toolTitle = nullptr;
    QLabel *m_toolDesc = nullptr;

    QStackedWidget *m_stack = nullptr;
    HomePage *m_home = nullptr;
    SettingsPage *m_settingsPage = nullptr;

    QWidget *m_toolPanel = nullptr;   ///< 当前内嵌面板（唯一，离开视图即销毁）
    QString m_currentToolId;

    QHash<QString, QPointer<ToolWindow>> m_toolWindows;   ///< 每工具最多一个独立窗口
    bool m_forceClose = false;
};

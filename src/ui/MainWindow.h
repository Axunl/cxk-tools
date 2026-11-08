#pragma once

// ---------------------------------------------------------------------------
// 主窗口：无边框自绘标题栏 + 左侧「搜索框 / 工具列表」+ 右侧工具面板。
//
// 关键设计：
//   * 工具面板用 QStackedWidget 缓存（m_pages: id -> index）。切换工具不销毁
//     面板，插件的输入内容因此能保留到进程退出 —— 这是产品需求，别改成重建。
//   * 关闭按钮只 hide() 收进托盘；真正退出要先 prepareQuit()。
//   * 搜索框在主窗口侧边栏里（不做独立浮层），实时过滤列表，回车/↑↓/Esc 都处理。
// ---------------------------------------------------------------------------

#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QString>

class ToolRegistry;
class Settings;
class PluginManager;
class SettingsPage;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(ToolRegistry *registry, Settings *settings, PluginManager *plugins, QWidget *parent = nullptr);

    /// 切到指定工具。
    /// @param countUsage 只有用户主动打开才累计使用频次；启动恢复上次工具传 false。
    void showTool(const QString &toolId, bool countUsage = true);

    void showSettings();

    /// 启动时恢复上次使用的工具；没有记录或记录失效则选第一个。
    void selectLastTool();

    /// 真正的退出：允许窗口关闭（否则 close() 只是收进托盘）。
    void prepareQuit();

    /// 呼出搜索：窗口前台显示并把焦点落到搜索框（热键/托盘"呼出搜索"都走这里）。
    void focusSearch();

    /// 设置页的热键状态回显（"已生效" / 失败原因）。
    void setHotkeyStatus(const QString &text, bool ok);

signals:
    void restartRequested();
    void hotkeyChanged(const QKeySequence &seq);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    QWidget *panelFor(const QString &toolId);
    void refreshSidebar();
    void applySidebarFilter(const QString &text);
    void moveSideSelection(int delta);
    void updateHeader(const QString &title, const QString &desc);

    ToolRegistry *m_registry = nullptr;
    Settings *m_settings = nullptr;
    PluginManager *m_plugins = nullptr;

    QWidget *m_titleBar = nullptr;
    QLabel *m_hotkeyHint = nullptr;   ///< 标题栏上的热键提示，改热键时要同步刷新
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_sideList = nullptr;
    QLabel *m_footer = nullptr;
    QLabel *m_toolTitle = nullptr;
    QLabel *m_toolDesc = nullptr;
    QStackedWidget *m_stack = nullptr;
    SettingsPage *m_settingsPage = nullptr;
    QWidget *m_emptyPage = nullptr;

    QHash<QString, int> m_pages;   ///< 工具 id -> QStackedWidget 索引（面板缓存）
    bool m_forceClose = false;
};

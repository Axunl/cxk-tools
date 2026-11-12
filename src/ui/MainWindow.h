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
//   * 「弹独立窗口」是**搬**不是**抄**：工具页里点页头「独立窗口」或双击标题栏，
//     当前内嵌面板会整块移交给独立窗口（同一个 QWidget 实例），正在编辑的内容、
//     光标、撤销栈、滚动位置全部原样过去，然后主窗口**收起**——弹出即把工作区
//     交给那个窗口，两个窗口同时在场只会互相抢焦点。最后一个独立窗口关掉时
//     主窗口再回来（前提是它当初就是为弹窗收起的）。详见 popOutTool()。
//   * 关闭按钮与 Esc 语义：首页 → 收进托盘；工具页/设置页 → 先回首页。
//     真正退出必须先 prepareQuit()，否则会被 closeEvent 拦下。
// ---------------------------------------------------------------------------

#include "IToolInput.h"

#include <QElapsedTimer>
#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QString>

class HomePage;
class LocalAppIndex;
class PluginManager;
class Settings;
class SettingsPage;
class ToolRegistry;
class ToolWindow;
class QGraphicsDropShadowEffect;
class QLabel;
class QPropertyAnimation;
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
    /// @param input 首页建议带进来的内容（JSON / 图片 / 待翻译文本）。
    ///              非空时要求插件实现了可选的 IToolInput，否则只打开工具、不带内容。
    void showTool(const QString &toolId, bool countUsage = true,
                  const xtools::ToolInput &input = xtools::ToolInput());

    /// 把工具弹成独立窗口。已有该工具的窗口时不新建，只前台化。
    ///
    /// 若该工具此刻正以内嵌面板的形式显示在主窗口里，面板会被**移交**给独立窗口
    /// （不是重新 createPanel），所以用户正在编辑的内容不会丢；移交后主窗口收起，
    /// 关掉最后一个独立窗口时再回来。
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

    /// 设置页「更新」分组的状态回显与检查中状态（转发给 SettingsPage）。
    void setUpdateStatus(const QString &text, bool ok);
    void setUpdateChecking(bool checking);

signals:
    void restartRequested();
    void hotkeyChanged(const QKeySequence &seq);
    /// 设置页点了「检查更新」：真正的联网与弹窗都在 AppController，界面层不自己发请求。
    void checkUpdateRequested();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void createToolPanel(const QString &toolId, const xtools::ToolInput &input);
    void destroyToolPanel();

    /// 把当前内嵌面板从主窗口**摘下来交给调用方**（不销毁，不重新创建）。
    /// @return 该工具的面板；当前显示的不是这个工具时返回 nullptr（调用方自行 createPanel）。
    /// 摘下来之后主窗口回首页：工具已经不在主窗口里了，留一个空壳工具页没有意义。
    QWidget *takeCurrentToolPanel(const QString &toolId);

    /// 标题栏「双击两下」的入口：只有当前在工具页时才真的弹窗（首页/设置页静默忽略）。
    void popOutCurrentTool();

    void updateHeader(const QString &title, const QString &desc);
    /// 记录一次「用户主动打开了这个工具」：频次 + 首页的最近使用。
    void markUsed(const QString &toolId);
    /// 启动本机应用；成功后同样记一笔频次/最近使用，然后收起主窗口。
    void launchLocalApp(const QString &appId);
    void closeAllToolWindows();
    void saveWindowState();

    /// 弹出独立窗口后把主窗口收起（hide，不是 close：不销毁面板、不进托盘逻辑）。
    void hideForPopout();
    /// 最后一个独立窗口关掉时把主窗口还回来——只认「当初是为弹窗收起的」，
    /// 用户自己收到托盘的窗口不会被别人的关闭动作翻出来。
    /// 显示排在下一轮事件循环（等独立窗口真正从系统里消失），随后做一次淡入。
    void restoreAfterPopout();
    /// 由隐到显的一次淡入（windowOpacity 0→1，170ms）。
    /// 桌面半透明窗口直接 show() 会「啪」地整块出现，在 Windows 上还可能先闪一帧
    /// 未合成的底色；淡入把这两件事都盖掉。热键/托盘唤出不走这里（要即时响应）。
    void fadeIn();

    /// 当前主题下的投影颜色（浅色偏浅、深色更不透明）。换肤时随 Theme::themeChanged 刷新。
    QColor shadowColor() const;

    ToolRegistry *m_registry = nullptr;
    Settings *m_settings = nullptr;
    PluginManager *m_plugins = nullptr;
    LocalAppIndex *m_apps = nullptr;

    QWidget *m_titleBar = nullptr;
    QLabel *m_hotkeyHint = nullptr;   ///< 标题栏上的热键提示，改热键时要同步刷新
    QGraphicsDropShadowEffect *m_shadow = nullptr;   ///< 主窗口投影，换肤时刷新颜色

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
    /// 主窗口当前是不是「为了弹独立窗口才收起的」。只有 true 时才在独立窗口全关后
    /// 自动回来；用户自己关窗（收托盘）或热键唤出都会把它清掉。
    bool m_hiddenForPopout = false;
    /// 由隐到显的淡入动画（windowOpacity）。懒创建，见 fadeIn()。
    QPropertyAnimation *m_fade = nullptr;

    /// 标题栏「两下」的自判定状态。为什么要自己算：第一次按下就把拖动交给了
    /// startSystemMove()，Windows 的原生移动循环会占住鼠标，随后的第二次按下
    /// 未必还会变成 Qt 的 MouseButtonDblClick（详见 eventFilter 里的说明）。
    QElapsedTimer m_titleClickTimer;   ///< 上一次按下的时刻（单调时钟，不受系统时间调整影响）
    QPoint m_titleClickPos;            ///< 上一次按下的位置，用来排除"两下"落在不同地方
    bool m_titleClickValid = false;    ///< 计时器是否有效（首次按下前为 false）
};

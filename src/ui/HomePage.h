#pragma once

// ---------------------------------------------------------------------------
// 首页：顶部搜索框 + 「最近使用 / 全部 / 本机应用」磁贴网格（uTools 式）。
//
// 关键设计：
//   * 磁贴是控件（LauncherTile）而不是 QListView + delegate：分组标题、空组隐藏、
//     右键菜单都是控件的天然能力，不必自己写 paint() 处理圆角/DPR/省略号。
//   * 磁贴尺寸固定（104×100），列数由可用宽度算出后手动重排 —— QGridLayout
//     不会自己决定列数，交给它拉伸只会让磁贴大小不一致。
//   * 列数计算恒定预留一条竖滚动条的宽度，否则会出现「少一列→变矮→滚动条消失→
//     多一列→变高→滚动条出现」的抖动循环。
//   * 两类数据源：工具（ToolRegistry）与本机应用（LocalAppIndex）。
//     空关键词时「全部」只列工具，应用只出现在「最近使用」里 —— 否则上百个
//     开始菜单项会把首页淹掉；输入关键词后工具与应用分两组显示搜索结果。
//   * 磁贴顺序 = 键盘导航顺序（最近 → 工具 → 应用），不做去重：同一项可能同时
//     出现在两组里（与 uTools 一致），保持「下标 = 视觉位置」，否则方向键会跳到
//     跟眼睛看到的不一致的地方。
// ---------------------------------------------------------------------------

#include <QIcon>
#include <QList>
#include <QString>
#include <QWidget>

class QGridLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class QHBoxLayout;
class LocalAppIndex;
class PluginManager;
class Settings;
class ToolRegistry;
struct ToolEntry;

/// 磁贴数据：工具与本机应用共用一套展示逻辑，只有 id 命名空间与右键菜单不同。
struct LauncherItem
{
    enum class Kind {
        Tool,   ///< 插件工具：点击在主窗口内打开
        App,    ///< 本机应用：点击交给系统启动
    };

    Kind kind = Kind::Tool;
    QString id;           ///< 工具 id 或 app:<hash>
    QString name;
    QString description;  ///< 工具的描述 / 应用的完整路径（作 tooltip）
    QIcon icon;
};

/// 首页磁贴：一个 104×100 的方块，48px 图标 + 单行名称（超长省略）。
///
/// 为什么自己画 hover/current 两次态而不是全靠 qss `:hover`：QWidget 子类的
/// hover 依赖样式引擎主动打 WA_Hover，不同 Qt 版本/平台样式下不一定触发；
/// 用动态属性 + repolish 是确定的。改这里时别把属性名和 qss 选择器改岔。
class LauncherTile : public QWidget
{
    Q_OBJECT

public:
    explicit LauncherTile(const LauncherItem &item, QWidget *parent = nullptr);

    QString id() const { return m_item.id; }
    LauncherItem::Kind kind() const { return m_item.kind; }

    /// 键盘导航的「当前项」高亮（焦点始终留在搜索框，所以不能靠 focus 表达）。
    void setCurrent(bool on);
    bool isCurrent() const { return m_current; }

signals:
    void activated(const QString &id);
    void popOutRequested(const QString &toolId);   ///< 仅工具：在独立窗口打开
    void revealRequested(const QString &appId);    ///< 仅应用：在文件管理器中显示

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void setHovered(bool on);
    void updateNameText();

    LauncherItem m_item;
    QString m_fullName;   ///< 未省略的名称，按当前字体宽度截断后再显示
    QIcon m_icon;         ///< 图标在 showEvent 里按真实 DPR 取像素，构造时拿不到正确 dpr
    QLabel *m_iconLabel = nullptr;
    QLabel *m_name = nullptr;
    bool m_current = false;
    bool m_hovered = false;
};

/// 首页页：搜索 + 网格 + 页脚。
class HomePage : public QWidget
{
    Q_OBJECT

public:
    HomePage(ToolRegistry *registry, Settings *settings, PluginManager *plugins,
             LocalAppIndex *apps, QWidget *parent = nullptr);

    /// 热键/托盘呼出：清空关键词、重建网格、聚焦并全选搜索框。
    void focusSearch();

    /// 设置关键词（调试/命令行 --search 用），会触发重建并聚焦搜索框。
    void setSearchText(const QString &text);

    /// 键盘导航：移动「当前项」delta 格（左右 ±1，上下 ±列数）。
    void moveCurrent(int delta);

    /// 键盘：打开当前高亮项；没有高亮时打开第一个。返回是否打开了某项。
    bool activateCurrent();

    int columnCount() const;

signals:
    void toolActivated(const QString &toolId);
    void toolPopOutRequested(const QString &toolId);
    void appActivated(const QString &appId);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void rebuild();
    void clearTiles();
    void reflow();
    int columnsFor(int viewportWidth) const;
    void placeTiles(QGridLayout *grid, const QList<LauncherTile *> &tiles, int columns);
    void setCurrentIndex(int index);
    void updateFooter();
    void showEmptyState(bool empty);

    ToolRegistry *m_registry = nullptr;
    Settings *m_settings = nullptr;
    PluginManager *m_plugins = nullptr;
    LocalAppIndex *m_apps = nullptr;

    QWidget *m_top = nullptr;   ///< 搜索框那一行，空工具时整行隐藏
    QLineEdit *m_search = nullptr;
    QToolButton *m_searchClear = nullptr;   ///< 框内右侧的 ✕，随文字显隐

    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QWidget *m_recentBox = nullptr;   ///< 「最近使用」整组（工具 + 应用混排），空则隐藏
    QLabel *m_recentTitle = nullptr;
    QGridLayout *m_recentGrid = nullptr;
    QWidget *m_allBox = nullptr;      ///< 「全部」或搜索时的工具结果
    QLabel *m_allTitle = nullptr;
    QGridLayout *m_allGrid = nullptr;
    QWidget *m_appBox = nullptr;      ///< 搜索时的「本机应用」结果
    QLabel *m_appTitle = nullptr;
    QGridLayout *m_appGrid = nullptr;

    QWidget *m_emptyBox = nullptr;   ///< 一个工具都没有时的引导块
    QLabel *m_noResult = nullptr;    ///< 搜索无命中的提示（与空工具区分开）
    QLabel *m_footer = nullptr;

    QList<LauncherTile *> m_tiles;   ///< 视觉顺序 = 键盘导航顺序
    QList<LauncherTile *> m_recentTiles;
    QList<LauncherTile *> m_allTiles;
    QList<LauncherTile *> m_appTiles;
    int m_currentIndex = -1;
    int m_lastColumns = -1;   ///< 列数没变就不重排，避免拖动窗口时反复闪
};

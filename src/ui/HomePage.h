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
//   * 方向键导航按**二维网格**算，不是按下标加减：分组各自从第 0 行起排，跨组
//     时的视觉位置与下标不连续（「最近使用」只剩半行时尤其明显）。见 moveCurrentBy。
//   * 网格**默认选中第一项**（呼出、输入、重建之后都是），回车与方向键都有落点。
//     但「默认选中」不等于「用户挑过」（m_currentByUser）：建议条与就地求值的回车
//     优先权只按用户是否真的挑过算，否则回车永远只打开第一项。
//   * 选中/悬停底色**自绘**（LauncherTile::paintEvent），不改动态属性、不做样式
//     重算：unpolish/polish 会连子控件一起重算样式并把几何标脏，方向键连按时每一步
//     都重排一次，观感是「按了跟不上」。
//   * 左右键与文本编辑共用：光标还没顶到这个方向的尽头、又没进入导航时，左右键
//     留给 QLineEdit 移光标；光标已经在尽头（或搜索框本来就空、或已用方向键选过
//     磁贴）时，左右键才用来换选中项 —— 抢走编辑中的光标键，改一个错字都得先按 Home。
//     带 Shift/Ctrl/Alt 的方向键一律不管（Ctrl+←/→ 整词跳、Shift+←/→ 扩选照常）。
//   * 数字键直选（详见 handleShortcutNumber 的注释与 README）：结果有两个以上时，
//     前 9 个磁贴按视觉顺序标 1~9；搜索框为空时裸数字直接打开第 N 项，搜索框有字
//     时裸数字照常输入（不打断 base64 / 360 这类关键词），改用 Ctrl+数字直选。
//   * 搜索框顶部会按输入内容给一条「建议条」（SearchIntent 区段）：贴进一段 JSON、
//     贴进一张图片、或输入英文却什么都没搜到时，建议条把「交给哪个工具、会带什么
//     内容进去」摆在最前面 —— 此时回车走建议，而不是走搜索结果。
//   * 算式就地求值：输入 12*8 时，结果以「= 96」的小徽标贴在搜索框右侧（不弹面板、
//     不列磁贴），回车或点徽标即复制到剪贴板。判定规则见 core/QuickCalc.h。
// ---------------------------------------------------------------------------

#include "IToolInput.h"
#include "QueryIntent.h"

#include <QIcon>
#include <QImage>
#include <QList>
#include <QString>
#include <QWidget>

class QGridLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QToolButton;
class QTimer;
class QVBoxLayout;
class QHBoxLayout;
class QMouseEvent;
class QEnterEvent;
class QShowEvent;
class QResizeEvent;
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
/// 选中/悬停两次态由 paintEvent 自绘，不用 qss 动态属性：动态属性要配
/// unpolish/polish 才会生效，而那一对调用会把磁贴连同子控件重算样式、顺带标脏
/// 几何，方向键连按时每一步都触发一次重排 —— 这正是「选了不跟手」的来源。
/// 自绘只花一次 update()，色值仍然走主题令牌，换肤时 Theme 会遍历 update()。
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

    /// 设置数字角标：1~9 显示在左上角，<=0 隐藏。
    ///
    /// 只负责「画」，按键由 HomePage 统一处理 —— 磁贴自己拿不到键盘事件
    /// （焦点一直在搜索框，也没有快捷键作用域可以挂）。
    void setShortcutNumber(int number);

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
    void paintEvent(QPaintEvent *event) override;

private:
    void setHovered(bool on);
    void updateNameText();

    LauncherItem m_item;
    QString m_fullName;   ///< 未省略的名称，按当前字体宽度截断后再显示
    QIcon m_icon;         ///< 图标在 showEvent 里按真实 DPR 取像素，构造时拿不到正确 dpr
    QLabel *m_iconLabel = nullptr;
    QLabel *m_name = nullptr;
    QLabel *m_shortcut = nullptr;   ///< 左上角的数字角标，不参与布局（绝对定位，见构造函数）
    int m_shortcutNumber = 0;       ///< 当前角标数字，0 = 不显示
    bool m_current = false;
    bool m_hovered = false;
};

/// 搜索建议条：横跨一行，说明「这段输入能交给哪个工具、会带什么内容进去」。
///
/// 为什么单独做成控件而不是塞进磁贴网格：磁贴是 104×100 的方块，放不下一句话说明；
/// 建议条需要显示「目标工具 + 内容摘要 + 回车提示」三段信息，且要能整行点击。
class IntentBanner : public QWidget
{
    Q_OBJECT

public:
    explicit IntentBanner(QWidget *parent = nullptr);

    /// 刷新内容。图标按工具取（与磁贴同一套兜底逻辑由调用方负责）。
    void setContent(const QIcon &icon, const QString &title, const QString &detail);

signals:
    void activated();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setHovered(bool on);
    /// 按建议条当前宽度把详情文字省略成一行（超长内容不撑宽布局、也不生硬截断）。
    void updateElidedDetail();

    QIcon m_icon;   ///< 图标在 showEvent 里按真实 DPR 取像素（与 LauncherTile 同一处理）
    QString m_detailText;   ///< 未省略的详情原文，每次 resize 都按新宽度重算
    QLabel *m_iconLabel = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_detail = nullptr;
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

    /// 键盘导航：在当前所在的二维网格上移动高亮项。
    /// dRow 垂直方向 ±1 行（跨越多行），dCol 水平方向 ±1 格（沿视觉顺序线性走）。
    /// 默认就选中第一项，所以按下方向键是「从第一项走到下一项」，不需要先点亮。
    void moveCurrentBy(int dRow, int dCol);

    /// 键盘：打开当前高亮项；没有高亮时打开第一个。返回是否打开了某项。
    bool activateCurrent();

    int columnCount() const;

signals:
    void toolActivated(const QString &toolId);
    void toolPopOutRequested(const QString &toolId);
    void appActivated(const QString &appId);
    /// 带内容进入工具（JSON 原文 / 粘贴的图片 / 待翻译英文）。
    /// 与 toolActivated 分开：普通打开不需要携带任何内容。
    void toolActivatedWithInput(const QString &toolId, const xtools::ToolInput &input);

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
    /// 只在高亮项**不在视口里**时滚一次，且滚到刚好可见就停。
    /// 不用 QScrollArea::ensureWidgetVisible：它每调用一次都会把目标贴到边缘（带内缩），
    /// 于是每按一下方向键滚动条都微调几像素，整片网格跟着抖。
    void scrollTileIntoView(LauncherTile *tile);
    /// 鼠标点开某个磁贴时同步高亮：点完接着按方向键，起点就是刚点的这一项
    /// （否则高亮停在上一次键盘位置，下一下就跳到看不见的地方）。
    void selectTileFromMouse(LauncherTile *tile);
    /// 非空分组按显示顺序列出（最近使用 → 全部 → 本机应用）。
    /// 与 m_tiles 的拼接顺序完全一致，所以「组内序号 + 各组长度偏移」就能换算出
    /// m_tiles 下标 —— 方向键导航要靠它算「这一行的相邻项是谁」。
    QList<QList<LauncherTile *>> tileGroups() const;
    void updateFooter();
    void showEmptyState(bool empty);

    // ------------------------------------------------------------ 数字键直选
    /// 按视觉顺序给前 9 个磁贴标号；结果只有一个时全部去掉角标（回车就是它）。
    /// 每次 rebuild() 都要重跑：磁贴是新建的，编号也跟着顺序变了。
    void applyShortcutNumbers();
    /// 打开第 number 项（1 起）。序号没有对应磁贴时返回 false，让按键回到搜索框当输入。
    bool activateNumber(int number);
    /// 发起「打开这个磁贴」：工具走 toolActivated，应用走 appActivated。
    void activateTile(LauncherTile *tile);

    // ------------------------------------------------------------ 搜索建议
    /// 按当前输入算出建议并刷新建议条；query 为空或建议的工具没加载时不显示。
    void updateIntent(const QString &query, bool hasToolHits);
    /// 执行建议：把内容装进 ToolInput 交给 MainWindow。回车与点击建议条都走这里。
    void activateIntent();
    /// 记下粘贴进来的图片并重建（图片优先于文字搜索）。
    void setPendingImage(const QImage &image);
    /// 丢掉待处理图片（清空搜索、Esc、切走、执行建议后都要调）。
    void clearPendingImage();
    /// 清空搜索框 + 待处理图片，并重建一次（✕ 按钮与 Esc 共用）。
    void clearSearch();
    /// 剪贴板里是图片时返回它，否则返回空 QImage。
    /// 某些截图工具只往剪贴板放 pixmap，只查 imageData() 会拿不到，所以要回退一次。
    static QImage clipboardImage();

    // ------------------------------------------------------------ 就地求值
    /// 按当前关键词重算算式结果并刷新搜索框内的徽标；没有可算的内容就整块隐藏。
    /// 由 rebuild() 统一调用（每次输入变化都会走到这里）。
    void updateInlineResult();
    /// 摆搜索框内的两个浮层：清空按钮（右内缘）与结果徽标（清空按钮左侧）。
    /// 两者的几何都完全由代码控制（它们在 QLineEdit 内部，没有布局可依）。
    void positionSearchOverlays();
    /// 把当前结果整段复制到剪贴板，并让徽标短暂显示「已复制」。
    void copyInlineResult();
    /// 切换徽标的「已复制」反馈态（动态属性 + repolish，qss 才会换色）。
    void setInlineFeedback(bool on);
    /// 按当前状态（结果文本 / 反馈态）重排徽标文字与尺寸。
    void refreshInlineBadge();

    ToolRegistry *m_registry = nullptr;
    Settings *m_settings = nullptr;
    PluginManager *m_plugins = nullptr;
    LocalAppIndex *m_apps = nullptr;

    QWidget *m_top = nullptr;   ///< 搜索框那一行，空工具时整行隐藏
    QLineEdit *m_search = nullptr;
    QToolButton *m_searchClear = nullptr;   ///< 框内右侧的 ✕，随文字显隐
    QLabel *m_inlineResult = nullptr;       ///< 框内右侧的算式结果徽标（「= 96」）
    QString m_inlineValue;                  ///< 当前结果的纯文本（复制用），空 = 无结果
    QTimer *m_inlineTimer = nullptr;        ///< 「已复制」反馈的复位定时器

    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    IntentBanner *m_intent = nullptr;         ///< 建议条，没有建议时整块隐藏
    queryintent::Suggestion m_intentData;     ///< 当前建议（内容 + 目标工具）
    QImage m_pendingImage;                    ///< 从剪贴板贴进来的图片，等待交给 OCR
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
    QLabel *m_shortcutHint = nullptr;   ///< 页脚右侧的数字键提示，没有角标时隐藏

    QList<LauncherTile *> m_tiles;   ///< 视觉顺序 = 键盘导航顺序
    QList<LauncherTile *> m_recentTiles;
    QList<LauncherTile *> m_allTiles;
    QList<LauncherTile *> m_appTiles;
    int m_currentIndex = -1;
    /// 「用户自己挑过磁贴」：方向键移动或点击后置 true，重建（打字/清空/唤出）时复位。
    /// 回车优先级靠它判断，不能靠 m_currentIndex —— 默认就选中第一项，下标永远有效。
    bool m_currentByUser = false;
    int m_lastColumns = -1;   ///< 列数没变就不重排，避免拖动窗口时反复闪
};

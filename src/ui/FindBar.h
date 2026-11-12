#pragma once

// ---------------------------------------------------------------------------
// 工具面板的通用查找条（Ctrl+F）。
//
// 定位：这是**宿主**提供的功能，不是某个插件自己实现的 —— 插件面板里大量
// QPlainTextEdit / QTextEdit（JSON、SQL、日志输出、OCR 结果…），逐个插件写
// 查找逻辑意味着 20+ 份重复代码。宿主在面板创建时给它挂一个 FindBar，
// 拦截面板范围内的 Ctrl+F，直接操作面板里的 QTextDocument，插件零改动。
//
// 关键设计：
//   * FindBar 是面板的**子控件**（浮层），随面板一起销毁；attach() 幂等 ——
//     「独立窗口接管面板」时面板上已经有一个，不能重复挂。
//   * Ctrl+F 用 qApp 级事件过滤器拦截：焦点在面板内任何子控件上时按键都
//     不会经过面板自身（事件先给焦点控件），挂面板级别的 filter 收不到。
//     过滤器里先判断按键目标是否在本面板子树内，面板外的按键原样放行。
//     macOS 上 Qt::ControlModifier 对应 Cmd 键，所以 Cmd+F 同样可用。
//   * 查找目标按「焦点优先」选：焦点在哪个编辑器里就搜哪个（textkit 这类
//     双编辑器面板的行为才符合直觉）；焦点不在编辑器上时退回面板里第一个
//     可见编辑器。只读的输出框也算合法目标（日志、OCR 结果更需要查找）。
//   * 颜色全走主题令牌（规则 7）：模板用 applyStyle 存在控件属性上，
//     换肤时 Theme 会重算，浅/深主题切换不需要重启。
//   * 面板里没有任何文本控件（如取色器）时 Ctrl+F 不显示查找条，静默忽略。
// ---------------------------------------------------------------------------

#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;
class QTextDocument;

class FindBar : public QWidget
{
    Q_OBJECT

public:
    /// 给面板挂查找条。幂等：已有 FindBar 的面板直接返回（独立窗口接管
    /// 主窗口面板的场景，面板随 reparent 一起带着原有的查找条走）。
    static void attach(QWidget *panel);

private:
    explicit FindBar(QWidget *panel);
    ~FindBar() override;

    void eventFilterOnApp();
    bool eventFilter(QObject *watched, QEvent *event) override;

    // ------------------------------------------------------------ 打开与目标选择
    /// 唤出查找条：选目标、显示、聚焦输入框并按当前关键词刷新。
    /// 面板里没有文本控件时直接返回，不显示。
    void open();
    /// 选定要搜索的编辑器：焦点所在编辑器优先，否则第一个可见编辑器。
    /// @param keepCurrent 焦点在查找条自己身上时（Ctrl+F 二次按下）保持现有目标。
    void pickTarget(bool keepCurrent);
    /// 切换搜索目标并重挂文档变化监听；传 nullptr 即清空（换目标时旧的匹配列表失效）。
    /// target 是 QPlainTextEdit 或 QTextEdit（两种控件没有公共编辑基类，
    /// 所以只收 QWidget 指针，具体类型在实现里解）。
    void setTarget(QWidget *target);

    // ------------------------------------------------------------ 查找逻辑
    /// 从头扫描目标文档收集所有匹配的起始位置（封顶 kMaxMatches，防止
    /// 超大文档在每次按键时把界面拖死）。
    void refreshMatches();
    /// 重建匹配列表并跳到光标附近的第一处，刷新计数标签。
    void refreshAndJump();
    /// 在匹配列表里前/后跳一格（循环回绕），选中并滚动到可见。
    void step(int direction);
    /// 把第 index 处匹配设为编辑器选区并滚动过去。
    void jumpTo(int index);
    /// 按当前状态（匹配数 / 当前序号）刷新计数标签文字与颜色态。
    void updateCountLabel(int index);
    /// 关闭查找条并把焦点还给编辑器（Esc 与 ✕ 按钮共用）。
    void closeBar();

    // ------------------------------------------------------------ 摆放
    /// 贴在面板右上角（面板是插件的自由布局，浮层位置只能由宿主控制）。
    void reposition();

    QWidget *m_panel = nullptr;   ///< 宿主面板（即 parent），随其销毁
    QLineEdit *m_input = nullptr;
    QLabel *m_count = nullptr;
    QToolButton *m_caseBtn = nullptr;   ///< 大小写开关（checkable）
    QToolButton *m_prevBtn = nullptr;   ///< 上一个
    QToolButton *m_nextBtn = nullptr;   ///< 下一个
    QToolButton *m_closeBtn = nullptr;  ///< 关闭

    QPointer<QWidget> m_targetWidget;   ///< 当前搜索目标（QPlainTextEdit 或 QTextEdit）
    QList<int> m_positions;             ///< 所有匹配的起始字符位置
    QString m_needle;                   ///< 上次重建列表时的关键词（失效判断用）
    QMetaObject::Connection m_docChanged;   ///< 目标文档变化时刷新计数；换目标时断开
};

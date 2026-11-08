#pragma once

// ---------------------------------------------------------------------------
// 文本处理面板（插件内部类，宿主只认 ITool）。
//
// 三个页签：
//   * 加引号 —— 多行文本 → `'1','2','3'`，可选引号种类 / 分隔符 / 去空行 / 去重，
//     输入即出结果；
//   * 文本对比 —— 左右各粘一段，**实时**标出行级差异（左红仅左、右绿仅右）与
//     行内改动区间（深色字符块），并给出「复制差异清单」；
//   * 差异提取 —— 左右各粘一段（每行一项），列出各自独有的值（例：
//     `1 3 4` / `1 4 2` → 只在左 `3`、只在右 `2`）。
//
// 所有状态都是实例成员：宿主不缓存面板，同一工具可能同时存在「内嵌」与「独立窗口」
// 两份（见 docs/PLUGIN.md 第 4 节），用静态变量会串状态。
//
// 算法全部在 TextLines.h / TextDiff.h（纯 QtCore，无 QWidget），这里只做
// 「取文本 → 调算法 → 上色」，所以那两份头文件可以单独做断言测试。
// ---------------------------------------------------------------------------

#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;

class TextKitPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TextKitPanel(QWidget *parent = nullptr);

private:
    // ---------------------------------------------------------------- 页面搭建

    /// 三个页签各自的搭建函数，返回可直接 addTab 的 QWidget（parent 都是本面板）。
    QWidget *buildQuoteTab();
    QWidget *buildDiffTab();
    QWidget *buildSetTab();

    // ---------------------------------------------------------------- 行为

    /// 加引号页：按当前选项重新生成结果。输入框 textChanged 与选项变化都会调它。
    void updateQuote();

    /// 差异提取页：读左右两侧 → 求差集 → 填两个结果框。输入或选项变化即重算。
    void updateSetDiff();

    /// 对比页的防抖入口：每次输入只是重置定时器，停手 kDiffDebounceMs 后才真正比较，
    /// 避免每敲一个字就跑一遍 LCS（大文本下会明显掉帧）。
    void scheduleDiff();
    void runDiff();

    /// 用剪贴板文本**替换**指定编辑框的内容；剪贴板为空或不可用时报错，不静默清空。
    void pasteInto(QPlainTextEdit *edit, QLabel *status);

    /// 写剪贴板并回读校验（QClipboard::setText 没有返回值，被别的进程占住时
    /// 只会静默失败，用户粘贴出来还是旧内容）。
    void copyToClipboard(const QString &text, const QString &label, QLabel *status);

    void setStatusText(QLabel *label, const QString &text, bool error = false);

    // ---------------------------------------------------------------- 控件

    QTabWidget *m_tabs = nullptr;

    // 加引号页
    QPlainTextEdit *m_quoteInput = nullptr;
    QPlainTextEdit *m_quoteOutput = nullptr;
    QComboBox      *m_quoteMark = nullptr;        ///< 引号种类（data 为空串表示不加引号）
    QComboBox      *m_quoteSeparator = nullptr;   ///< 分隔符（data 是真实分隔串）
    QCheckBox      *m_quoteTrim = nullptr;
    QCheckBox      *m_quoteDropEmpty = nullptr;
    QCheckBox      *m_quoteUnique = nullptr;
    QLabel         *m_quoteStatus = nullptr;

    // 对比页
    QPlainTextEdit *m_leftEdit = nullptr;
    QPlainTextEdit *m_rightEdit = nullptr;
    QLabel         *m_leftCaption = nullptr;      ///< 「文本 A · 12 行」，比较后刷新行数
    QLabel         *m_rightCaption = nullptr;
    QLabel         *m_diffStatus = nullptr;
    QTimer         *m_diffTimer = nullptr;

    /// 最近一次比较生成的差异清单（`-` / `+` 逐行列出）。存成文本而不是结果对象，
    /// 是为了让头文件不必 include TextDiff.h；点「复制差异清单」时直接用它。
    QString m_lastDiffReport;

    // 差异提取页
    QPlainTextEdit *m_setLeftEdit = nullptr;      ///< 文本 A（可编辑）
    QPlainTextEdit *m_setRightEdit = nullptr;     ///< 文本 B
    QPlainTextEdit *m_onlyLeft = nullptr;
    QPlainTextEdit *m_onlyRight = nullptr;
    QCheckBox      *m_setIgnoreEmpty = nullptr;
    QCheckBox      *m_setByCount = nullptr;
    QLabel         *m_setStatus = nullptr;
    QLabel         *m_leftCountLabel = nullptr;   ///< 「只在左侧 · 3 项」
    QLabel         *m_rightCountLabel = nullptr;
    QPushButton    *m_copyLeftBtn = nullptr;
    QPushButton    *m_copyRightBtn = nullptr;
    QPushButton    *m_copyAllBtn = nullptr;
};

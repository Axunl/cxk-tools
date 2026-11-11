#pragma once

// ---------------------------------------------------------------------------
// SQL 工具面板（插件内部类，宿主只认 ITool）。
//
// 三个页签，对应 ssformat.com 那套工具的三种用法：
//   1. MyBatis 日志还原 —— 粘日志里的 `Preparing:` / `Parameters:` 两行，把 ? 填成实际值；
//   2. SQL 格式化      —— 子句换行 + 缩进 + 关键字大写；
//   3. SQL 压缩        —— 去注释、折空白，压成一行。
//
// 所有状态都是实例成员：宿主不缓存面板，同一工具可能同时存在「内嵌」与「独立窗口」
// 两份（见 docs/PLUGIN.md 第 4 节），用静态变量会串状态。
//
// 算法全在 SqlText.h（纯 QtCore、无 QWidget），这里只做「取文本 → 调算法 → 显示」。
// ---------------------------------------------------------------------------

#include <QWidget>

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QVBoxLayout;

class SqlPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SqlPanel(QWidget *parent = nullptr);

    /// 首页把内容带进来时调用（见 src/api/IToolInput.h）。
    /// 带日志标记的走「MyBatis 还原」，其余当普通 SQL 格式化，省得用户自己搬来搬去。
    void setInputText(const QString &text);

private:
    // ---------------------------------------------------------------- 页面搭建

    QWidget *buildMyBatisTab();
    QWidget *buildFormatTab();
    QWidget *buildCompressTab();

    /// 在工具条右边补上「复制结果 / 清空」两个按钮、弹性空白与状态行。
    /// 调用方先放主按钮和选项，再调它，保证三个页签的按钮次序一致。
    void addResultButtons(QHBoxLayout *row, QWidget *page, QPlainTextEdit *input,
                          QPlainTextEdit *output, QLabel *status);

    /// 输入 / 输出上下分栏。输入可编辑，输出只读，中间可拖动调节高度。
    void addPanes(QWidget *page, QVBoxLayout *layout, QPlainTextEdit **inputOut,
                  QPlainTextEdit **outputOut, const QString &inputHint, const QString &outputHint);

    // ---------------------------------------------------------------- 行为

    void runMyBatis();
    void runFormat();
    void runCompress();

    /// 写状态行：成功用正文色，失败用危险色并让用户看清原因。
    void setStatus(QLabel *label, const QString &text, bool error = false);

    // ---------------------------------------------------------------- 控件

    QTabWidget *m_tabs = nullptr;

    // MyBatis 还原页
    QPlainTextEdit *m_logInput = nullptr;
    QPlainTextEdit *m_logOutput = nullptr;
    QCheckBox      *m_logFormat = nullptr;   ///< 还原后顺手格式化
    QCheckBox      *m_logUpper = nullptr;    ///< 关键字大写（仅在格式化时生效）
    QLabel         *m_logStatus = nullptr;

    // 格式化页
    QPlainTextEdit *m_fmtInput = nullptr;
    QPlainTextEdit *m_fmtOutput = nullptr;
    QCheckBox      *m_fmtUpper = nullptr;
    QCheckBox      *m_fmtOneColumn = nullptr;
    QLabel         *m_fmtStatus = nullptr;

    // 压缩页
    QPlainTextEdit *m_minInput = nullptr;
    QPlainTextEdit *m_minOutput = nullptr;
    QLabel         *m_minStatus = nullptr;
};

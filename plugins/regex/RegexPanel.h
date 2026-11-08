#pragma once

// ---------------------------------------------------------------------------
// 正则表达式面板（插件内部类，宿主只认 ITool）。
//
// 界面自上而下四段：
//   1. 正则输入框 + 「常用正则」菜单 + 5 个标志复选框（i / m / s / x / U）；
//   2. 测试文本（可编辑、等宽字体），所有匹配就地高亮；
//   3. 匹配结果表：序号 / 行号 / 位置 / 匹配内容 / 捕获组，点一行跳到文本里对应位置；
//   4. 状态行：匹配数量与耗时，或编译错误（附出错字符位置）。
//
// 交互约定：
//   * 输入即匹配（防抖 120ms），没有「执行」按钮；
//   * 文本与结果共用一条竖向 splitter：长文本或长结果列表都能自己拖动比例；
//   * 统计上限 2000 条、循环内 1.5 秒预算（见 RegexMatch.h 的 Options），
//     超出会明确写在状态行里，不假装「就这么多」。
//
// 所有状态都是实例成员：宿主不缓存面板，同一工具可能同时存在「内嵌」与
// 「独立窗口」两份（见 docs/PLUGIN.md 第 4 节），用静态变量会互相串状态。
//
// 匹配算法全部在 RegexMatch.h（纯 QtCore，无 QWidget），这里只做
// 「取文本 → 调算法 → 填表 + 上色」，所以那份头文件可以单独做断言测试。
// ---------------------------------------------------------------------------

#include "RegexMatch.h"

#include <QVector>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

class RegexPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RegexPanel(QWidget *parent = nullptr);

private:
    /// 状态行三种语义：提示（灰）、正常结果（绿）、错误（红）。
    enum class StatusKind { Hint, Ok, Error };

    // ---------------------------------------------------------------- 页面搭建

    /// 「测试文本」区块（标题行 + 编辑框），返回的 QWidget 塞进 splitter 上半部分。
    QWidget *buildSubjectSide();

    /// 「匹配结果」区块（标题行 + 表格），塞进 splitter 下半部分。
    QWidget *buildResultSide();

    // ---------------------------------------------------------------- 行为

    /// 防抖入口：输入/勾选变化只是重置定时器，停手 kDebounceMs 后才真正匹配，
    /// 避免每敲一个字符就重复编译正则、重建整张表。
    void scheduleMatch();

    /// 真正执行：读界面状态 → regexmatch::run → applyResult。
    void runMatch();

    /// 把一次匹配结果落到界面：错误只写状态行；成功则填表、上色、写统计。
    void applyResult(const regexmatch::Result &result);

    /// 按 m_matches 重建表格内容（最多 2000 行，与 Options::maxMatches 一致）。
    void fillTable();

    /// 用 extraSelections 给文本上色：全部匹配淡黄，当前定位的那条深黄。
    /// 不改动字符格式，文本仍可正常编辑、选中、复制。
    void highlightMatches();

    /// 点表格某行 → 在文本里选中该匹配并滚动到可见位置。
    void locateMatch(int row);

    void clearHighlight();

    // ---------------------------------------------------------------- 小动作

    void copyAllMatches();
    void pasteSubject();
    void clearSubject();
    void updateSubjectInfo();
    void setStatus(const QString &text, StatusKind kind);

    // ---------------------------------------------------------------- 控件

    QLineEdit      *m_pattern = nullptr;   ///< 正则输入（等宽字体）
    QCheckBox      *m_case = nullptr;      ///< i
    QCheckBox      *m_multiline = nullptr; ///< m
    QCheckBox      *m_dotAll = nullptr;    ///< s
    QCheckBox      *m_extended = nullptr;  ///< x
    QCheckBox      *m_inverted = nullptr;  ///< U

    QPlainTextEdit *m_subject = nullptr;   ///< 测试文本
    QLabel         *m_subjectInfo = nullptr; ///< 「12 行 · 340 字符」
    QTableWidget   *m_table = nullptr;
    QPushButton    *m_copyAll = nullptr;
    QLabel         *m_status = nullptr;
    QTimer         *m_timer = nullptr;     ///< 防抖定时器

    QVector<regexmatch::MatchItem> m_matches; ///< 最近一次匹配结果，供填表/定位/复制复用
    int  m_currentRow = -1;                ///< 表格里当前定位到的行（-1 = 没有）
    bool m_fillingTable = false;           ///< 防止 setRowCount 触发的 currentCellChanged 反向定位
};

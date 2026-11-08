#pragma once

// ---------------------------------------------------------------------------
// 正则匹配的纯逻辑层（只依赖 QtCore，不 include 任何 QWidget）。
//
// 单独抽成头文件的两个理由：
//   1) 规则 2 要求改动必须有可执行的验证。临时工程 include 这个头文件就能对
//      「编译失败文案 / 出错位置 / 行号 / 捕获组 / 上限截断」逐条断言，不用起界面；
//   2) 面板只做「取文本 → 调 run() → 上色」，逻辑和渲染分开后两边都好改。
//
// 语法说明：Qt 底层是 PCRE2，不是浏览器里的 JS RegExp。JS 的 \u4e00 在这里要写成
// \x{4e00}；后行断言必须是定长（(?!...) / (?<=...) 支持，但 (?<=a|bc) 这类变长会报错）。
// 面板会在编译失败时把这条差异提示给用户，见 RegexPanel::applyResult。
// ---------------------------------------------------------------------------

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace regexmatch {

/// 单条匹配结果。
///
/// 位置统一用 QString 的 UTF-16 下标（0 基），与 QTextCursor / QTextDocument 的
/// 坐标系一致，面板拿到就能直接 setPosition，不需要换算。
struct MatchItem
{
    int start = 0;        ///< 起始下标（0 基）
    int length = 0;       ///< 匹配长度；0 表示零宽匹配（如 \b、^）
    int line = 1;         ///< 起始位置所在行号（1 基），长文本里定位用
    QString text;         ///< 匹配到的原文
    QStringList groups;   ///< 捕获组 1..n 的文本；未参与匹配的组是空串
};

/// 匹配参数。界面上的 5 个复选框就是对这里 5 个布尔值的包装。
struct Options
{
    bool caseInsensitive = false;    ///< i：忽略大小写
    bool multiline = false;          ///< m：^ $ 匹配每行首尾
    bool dotAll = false;             ///< s：. 也匹配换行
    bool extended = false;           ///< x：忽略空白与 # 注释（便于写长正则）
    bool invertedGreedy = false;     ///< U：量词默认非贪婪（贪婪反转）

    /// 最多收集多少条匹配。
    /// 上限不是为了省内存，而是防止 (.*) 这类在长文本上产出几十万条把表格和
    /// 高亮拖死；超过就停下并标记 truncated，界面如实告诉用户「只统计了前 N 条」。
    int maxMatches = 2000;

    /// 累计耗时预算（毫秒）。每收集一条检查一次，超了就中断。
    /// 注意：它挡不住**单次** match 内部的灾难性回溯（Qt 不提供匹配超时），
    /// 那种情况只能等 PCRE2 自己返回，这是已知边界。
    int timeBudgetMs = 1500;
};

/// 一次匹配的完整结果。界面只依赖这个结构，不碰 QRegularExpression。
struct Result
{
    bool ok = true;              ///< 正则是否编译成功
    bool emptyPattern = false;   ///< 模式为空：没做任何事，界面该显示提示而不是「0 个匹配」
    QString error;               ///< ok = false 时的编译错误文案
    int errorOffset = 0;         ///< 出错的大致字符位置（0 基），界面显示成 1 基
    QVector<MatchItem> matches;  ///< 按出现顺序排列，最多 Options::maxMatches 条
    bool truncated = false;      ///< 达到条数上限而提前停止
    bool timedOut = false;       ///< 达到时间预算而提前停止
    double elapsedMs = 0.0;      ///< 编译 + 匹配的总耗时，状态行展示
};

/// 把界面选项翻译成 Qt 的 PatternOptions。
inline QRegularExpression::PatternOptions toPatternOptions(const Options &options)
{
    QRegularExpression::PatternOptions flags = QRegularExpression::NoPatternOption;
    if (options.caseInsensitive)
        flags |= QRegularExpression::CaseInsensitiveOption;
    if (options.multiline)
        flags |= QRegularExpression::MultilineOption;
    if (options.dotAll)
        flags |= QRegularExpression::DotMatchesEverythingOption;
    if (options.extended)
        flags |= QRegularExpression::ExtendedPatternSyntaxOption;
    if (options.invertedGreedy)
        flags |= QRegularExpression::InvertedGreedinessOption;
    return flags;
}

///
/// 编译 pattern 并在 subject 上找出全部匹配。
///
/// 失败语义：
///   * pattern 为空 → ok = true + emptyPattern = true（空正则能匹配任何位置，
///     对用户没意义，所以不执行，交给界面提示）；
///   * 编译失败 → ok = false + error / errorOffset，不抛异常；
///   * 达到条数上限或时间预算 → ok = true，matches 是已收集的部分，
///     truncated / timedOut 说明原因。
///
/// 行号用「每行起始下标 + 二分」算：对每条匹配都从头扫一遍文本是 O(匹配数 × 文本长度)，
/// 几千条匹配的长日志上会明显卡顿。
///
inline Result run(const QString &pattern, const QString &subject, const Options &options)
{
    Result result;
    if (pattern.isEmpty()) {
        result.emptyPattern = true;
        return result;
    }

    QElapsedTimer timer;
    timer.start();

    const QRegularExpression re(pattern, toPatternOptions(options));
    if (!re.isValid()) {
        result.ok = false;
        result.error = re.errorString();
        result.errorOffset = re.patternErrorOffset();
        result.elapsedMs = double(timer.nsecsElapsed()) / 1'000'000.0;
        return result;
    }

    QVector<int> lineStarts{0};
    for (int i = 0; i < subject.size(); ++i) {
        if (subject.at(i) == QLatin1Char('\n'))
            lineStarts.append(i + 1);
    }

    const int maxMatches = qMax(1, options.maxMatches);
    const int timeBudget = qMax(1, options.timeBudgetMs);

    QRegularExpressionMatchIterator it = re.globalMatch(subject);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        if (!match.hasMatch())
            break;   // 防御：迭代器空转会导致死循环，宁可少统计也不卡住界面

        MatchItem item;
        item.start = match.capturedStart();
        item.length = match.capturedLength();
        item.text = match.captured(0);
        const QStringList captured = match.capturedTexts();
        for (int i = 1; i < captured.size(); ++i)
            item.groups.append(captured.at(i));

        // upper_bound 找到第一个「行首 > start」，它前面的行就是本条匹配所在行；
        // 返回的下标正好等于「行首 <= start 的个数」= 行号（1 基）。
        const auto lineIt = std::upper_bound(lineStarts.cbegin(), lineStarts.cend(), item.start);
        item.line = int(lineIt - lineStarts.cbegin());

        result.matches.append(item);

        if (result.matches.size() >= maxMatches) {
            // 还能不能有下一条未知，保守地标记为「已截断」让界面提示
            result.truncated = it.hasNext();
            break;
        }
        if (timer.elapsed() >= timeBudget) {
            result.timedOut = true;
            break;
        }
    }

    result.elapsedMs = double(timer.nsecsElapsed()) / 1'000'000.0;
    return result;
}

} // namespace regexmatch

#pragma once

// ---------------------------------------------------------------------------
// 行级 + 字符级文本差异（只依赖 QtCore，可单独 include 做断言测试）。
//
// 用途：两段「很相似」的文字，找出哪几行不同、同一行里又是哪几个字不同。
// 算法是 LCS（最长公共子序列）动态规划，回溯时从前往后走，得到一串
// 相等 / 仅左 / 仅右 的原始操作；再把「连续的仅左 + 仅右」里相似度够高的行
// 配成「修改」，对配上的那一对做第二次字符级 LCS，得到行内高亮区间。
//
// 为什么不用 Myers：这里的输入来自用户手工粘贴（几行到几千行），O(n·m) 的 DP
// 在 4M 格以内一次几十毫秒，代码短、无递归、不会因为最坏情况退化成指数；
// 真正需要处理超大文件时应该换成 Myers + 分块，但那不是这个工具的场景。
// ---------------------------------------------------------------------------

#include "TextLines.h"

#include <QChar>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace textdiff {

/// 一行（一对行）的差异类型。
enum class Kind
{
    Equal,    ///< 两侧相同
    Removed,  ///< 只在左侧
    Added,    ///< 只在右侧
    Changed,  ///< 两侧都有但内容不同（成对配对后）
};

/// 差异结果里的一行。未参与比较的另一侧，行号与文本都留空（-1 / 空串）。
struct Row
{
    Kind    kind = Kind::Equal;
    int     leftLine = -1;      ///< 左侧行号（0 基）
    int     rightLine = -1;     ///< 右侧行号（0 基）
    QString leftText;
    QString rightText;

    /// 行内需要加深标出的字符区间（起点 + 长度，相对该行文本），只对 Changed 行有值。
    QVector<QPair<int, int>> leftMarks;
    QVector<QPair<int, int>> rightMarks;
};

/// 一次比较的完整结果。
struct Result
{
    QVector<Row> rows;          ///< 按原文顺序排列，左右两侧的行一一对应
    int  equalLines = 0;
    int  changedLines = 0;      ///< 成对修改的行数
    int  removedLines = 0;      ///< 只在左侧的行数
    int  addedLines = 0;        ///< 只在右侧的行数
    int  leftLines = 0;         ///< 左侧总行数
    int  rightLines = 0;        ///< 右侧总行数
    bool approximate = false;   ///< true = 文本过大，退化成按行号对齐的粗略比较

    int differenceCount() const { return changedLines + removedLines + addedLines; }
};

// ---------------------------------------------------------------- 内部实现

namespace detail {

/// 超过这个行数或 DP 格数就退化比较：9MB 量级的 int 表还能接受，
/// 再大就不是「粘贴两段文字」的场景了，宁可给出一条明确的降级说明。
constexpr int kMaxLines = 4000;
constexpr qint64 kMaxCells = 4000000;

/// 行内（字符级）LCS 的规模上限：单行超过 4000 字符、或差异段乘积超过 25 万格时
/// 不再逐字比较，直接把整条差异段标出来 —— 结果一样能看懂，但不会卡住界面。
constexpr int kMaxInlineLength = 4000;
constexpr qint64 kMaxInlineCells = 250000;

/// 一个差异块里「删行 × 增行」的配对判断：
///   * kMinPairScore —— 相似度低于这个值就不认为是一处「修改」，宁可分开报仅左/仅右；
///   * kMaxPairCandidates —— 块内组合数超过它就放弃相似度评估，退化成按顺序配对，
///     免得一个几百行的整段替换把界面拖住。
constexpr double kMinPairScore = 0.4;
constexpr qint64 kMaxPairCandidates = 10000;

/// 相似度计算里「中间段 LCS」的格数上限：超过就只按前后缀粗估。
/// 2 万格对应 140 字符 × 140 字符，已经覆盖「粘贴过来的普通一行」的绝大多数情况。
constexpr qint64 kMaxSimilarityCells = 20000;

/// LCS 长度表：`dp[i * width + j]` = a[i..] 与 b[j..] 的最长公共子序列长度。
///
/// 反着填（从右下往左上）是为了让回溯能**从前往后**走，这样输出天然按原文顺序，
/// 不用把结果翻一遍；`width = b.size() + 1`，末行末列留 0 作为边界。
inline QVector<int> lcsTable(const QStringList &a, const QStringList &b)
{
    const int n = a.size();
    const int m = b.size();
    const int width = m + 1;
    QVector<int> dp((n + 1) * width, 0);
    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            if (a.at(i) == b.at(j))
                dp[i * width + j] = dp[(i + 1) * width + (j + 1)] + 1;
            else
                dp[i * width + j] = qMax(dp[(i + 1) * width + j], dp[i * width + (j + 1)]);
        }
    }
    return dp;
}

/// 字符级 LCS 长度表，规则同上，只是比较单位换成 QChar。
inline QVector<int> lcsTableChars(const QString &a, const QString &b)
{
    const int n = a.size();
    const int m = b.size();
    const int width = m + 1;
    QVector<int> dp((n + 1) * width, 0);
    for (int i = n - 1; i >= 0; --i) {
        for (int j = m - 1; j >= 0; --j) {
            if (a.at(i) == b.at(j))
                dp[i * width + j] = dp[(i + 1) * width + (j + 1)] + 1;
            else
                dp[i * width + j] = qMax(dp[(i + 1) * width + j], dp[i * width + (j + 1)]);
        }
    }
    return dp;
}

inline void appendMark(QVector<QPair<int, int>> *marks, int start, int length)
{
    if (length <= 0)
        return;
    marks->append(qMakePair(start, length));
}

///
/// 两行的相似度（0~1），用来判断「这两行算一处修改，还是各算各的」。
///
/// 度量方式：掐掉公共前缀/后缀，再对中间那段做字符级 LCS，
/// 取「公共字符数 ÷ 较长行的长度」。
///   * 只改了几个字（`version: 0.1.0` → `0.2.0`）：前缀覆盖大部分，分数很高；
///   * 中间一处替换（`SELECT * FROM ... = 1` → `SELECT name FROM ... = 2`）：
///     前缀覆盖不到，但 LCS 能把中间大段相同的字符算进来，分数同样很高；
///   * 毫不相干的两行：分数接近 0，不会被配成「修改」。
///
/// 中间段的 LCS 有格数上限：块内要两两比较，超长行会让代价炸开，
/// 这种情况下退回「只看前后缀」的粗估（仍足以区分「几乎一样」和「完全不像」）。
inline double similarity(const QString &a, const QString &b)
{
    const int length = qMax(a.size(), b.size());
    if (length == 0)
        return 1.0;

    const int shorter = qMin(a.size(), b.size());
    int prefix = 0;
    while (prefix < shorter && a.at(prefix) == b.at(prefix))
        ++prefix;

    int suffix = 0;
    // 前缀与后缀不能重叠，否则「全同的两行」会被算成 2 倍长度
    while (suffix < shorter - prefix
           && a.at(a.size() - 1 - suffix) == b.at(b.size() - 1 - suffix)) {
        ++suffix;
    }

    const int aLen = a.size() - prefix - suffix;
    const int bLen = b.size() - prefix - suffix;
    int common = prefix + suffix;
    if (aLen > 0 && bLen > 0 && qint64(aLen) * qint64(bLen) <= kMaxSimilarityCells) {
        // 表是反着填的，dp[0] 即整段的 LCS 长度
        const QVector<int> dp = lcsTableChars(a.mid(prefix, aLen), b.mid(prefix, bLen));
        common += dp.at(0);
    }
    return double(common) / double(length);
}

///
/// 给一对「修改行」算出行内高亮区间。
///
/// 先掐掉公共前缀与公共后缀，只在中间那段做字符级 LCS：用户改的通常就几个字，
/// 掐掉两端能把 LCS 的规模从「整行×整行」压到「几个字×几个字」。
/// 纯插入/纯删除（一侧中间段为空）或规模过大时，直接把整段标出来。
inline void markInline(Row *row)
{
    const QString &a = row->leftText;
    const QString &b = row->rightText;
    if (a == b)
        return;

    const int shorter = qMin(a.size(), b.size());
    int prefix = 0;
    while (prefix < shorter && a.at(prefix) == b.at(prefix))
        ++prefix;

    int suffix = 0;
    while (suffix < shorter - prefix
           && a.at(a.size() - 1 - suffix) == b.at(b.size() - 1 - suffix)) {
        ++suffix;
    }

    const int aLen = a.size() - prefix - suffix;
    const int bLen = b.size() - prefix - suffix;
    if (aLen <= 0 && bLen <= 0)
        return;

    if (aLen <= 0 || bLen <= 0 || aLen > kMaxInlineLength || bLen > kMaxInlineLength
        || qint64(aLen) * qint64(bLen) > kMaxInlineCells) {
        appendMark(&row->leftMarks, prefix, aLen);
        appendMark(&row->rightMarks, prefix, bLen);
        return;
    }

    const QString midA = a.mid(prefix, aLen);
    const QString midB = b.mid(prefix, bLen);
    const QVector<int> dp = lcsTableChars(midA, midB);
    const int width = bLen + 1;

    int i = 0;
    int j = 0;
    int runA = -1;   // 左侧未匹配连续段的起点
    int runB = -1;
    while (i < aLen && j < bLen) {
        if (midA.at(i) == midB.at(j)) {
            if (runA >= 0) {
                appendMark(&row->leftMarks, prefix + runA, i - runA);
                runA = -1;
            }
            if (runB >= 0) {
                appendMark(&row->rightMarks, prefix + runB, j - runB);
                runB = -1;
            }
            ++i;
            ++j;
        } else if (dp[(i + 1) * width + j] >= dp[i * width + (j + 1)]) {
            if (runA < 0)
                runA = i;
            ++i;
        } else {
            if (runB < 0)
                runB = j;
            ++j;
        }
    }
    // 收尾：一侧先走完时，另一侧剩下的整段就是未匹配部分。
    if (i < aLen && runA < 0)
        runA = i;
    if (j < bLen && runB < 0)
        runB = j;
    if (runA >= 0)
        appendMark(&row->leftMarks, prefix + runA, aLen - runA);
    if (runB >= 0)
        appendMark(&row->rightMarks, prefix + runB, bLen - runB);
}

/// 文本过大时的降级比较：不做 LCS，按行号一一对齐。
///
/// 缺点是把「中间插入了一行」误报成「后面每行都变了」，所以结果里要带
/// approximate 标记，让界面明说这是粗略结果，而不是假装精确。
inline Result compareApproximate(const QStringList &left, const QStringList &right)
{
    Result result;
    result.approximate = true;
    result.leftLines = left.size();
    result.rightLines = right.size();

    const int shared = qMin(left.size(), right.size());
    for (int i = 0; i < shared; ++i) {
        Row row;
        row.leftLine = i;
        row.rightLine = i;
        row.leftText = left.at(i);
        row.rightText = right.at(i);
        if (row.leftText == row.rightText) {
            row.kind = Kind::Equal;
            ++result.equalLines;
        } else {
            row.kind = Kind::Changed;
            ++result.changedLines;
            markInline(&row);
        }
        result.rows.append(row);
    }
    for (int i = shared; i < left.size(); ++i) {
        Row row;
        row.kind = Kind::Removed;
        row.leftLine = i;
        row.leftText = left.at(i);
        ++result.removedLines;
        result.rows.append(row);
    }
    for (int i = shared; i < right.size(); ++i) {
        Row row;
        row.kind = Kind::Added;
        row.rightLine = i;
        row.rightText = right.at(i);
        ++result.addedLines;
        result.rows.append(row);
    }
    return result;
}

} // namespace detail

// ---------------------------------------------------------------- 入口

///
/// 比较两段文本，返回逐行的差异结果。
///
/// 步骤：
///   1. 按行切分（三平台行尾都认，末尾换行不算空行）；
///   2. 规模超限 → 退化成按行号对齐的粗略比较（结果带 approximate=true）；
///   3. 行级 LCS 得到「相等 / 仅左 / 仅右」的操作序列；
///   4. 把连续的「仅左 + 仅右」块内的行按**相似度**贪心配对成「修改」，
///      配不上的（或太不像的）保留为仅左 / 仅右；
///   5. 对每个「修改」行做字符级 LCS，算出两侧要加深标出的字符区间。
///
/// 第 4 步为什么不按行号顺序硬配：两段文字里常见的「删两行 + 增两行」往往是
/// 其中一行改了几个字、另一行是新增内容，顺序配对会把它配成一处毫无意义的「修改」，
/// 行内高亮跟着变成整行乱码。看相似度只配对真像的两行，结果更接近人眼判断。
inline Result compare(const QString &leftText, const QString &rightText)
{
    const QStringList left = textlines::splitLines(leftText);
    const QStringList right = textlines::splitLines(rightText);

    if (left.size() > detail::kMaxLines || right.size() > detail::kMaxLines
        || qint64(left.size()) * qint64(right.size()) > detail::kMaxCells) {
        return detail::compareApproximate(left, right);
    }

    Result result;
    result.leftLines = left.size();
    result.rightLines = right.size();

    // ---- 第 1 步：回溯出原始操作序列（先不配对，也不做字符级比较）
    QVector<Row> raw;
    raw.reserve(left.size() + right.size());
    const QVector<int> dp = detail::lcsTable(left, right);
    const int width = right.size() + 1;

    int i = 0;
    int j = 0;
    while (i < left.size() && j < right.size()) {
        if (left.at(i) == right.at(j)) {
            Row row;
            row.kind = Kind::Equal;
            row.leftLine = i;
            row.rightLine = j;
            row.leftText = left.at(i);
            row.rightText = right.at(j);
            raw.append(row);
            ++i;
            ++j;
        } else if (dp[(i + 1) * width + j] >= dp[i * width + (j + 1)]) {
            Row row;
            row.kind = Kind::Removed;
            row.leftLine = i;
            row.leftText = left.at(i);
            raw.append(row);
            ++i;
        } else {
            Row row;
            row.kind = Kind::Added;
            row.rightLine = j;
            row.rightText = right.at(j);
            raw.append(row);
            ++j;
        }
    }
    while (i < left.size()) {
        Row row;
        row.kind = Kind::Removed;
        row.leftLine = i;
        row.leftText = left.at(i);
        raw.append(row);
        ++i;
    }
    while (j < right.size()) {
        Row row;
        row.kind = Kind::Added;
        row.rightLine = j;
        row.rightText = right.at(j);
        raw.append(row);
        ++j;
    }

    // ---- 第 2 步：把「连续的非相等段」里的删行/增行两两配对成「修改」
    int index = 0;
    while (index < raw.size()) {
        if (raw.at(index).kind == Kind::Equal) {
            ++result.equalLines;
            result.rows.append(raw.at(index));
            ++index;
            continue;
        }

        QVector<int> removed;
        QVector<int> added;
        while (index < raw.size() && raw.at(index).kind != Kind::Equal) {
            if (raw.at(index).kind == Kind::Removed)
                removed.append(index);
            else
                added.append(index);
            ++index;
        }

        const int pairs = qMin(removed.size(), added.size());
        if (pairs > 0) {
            // 配对策略：不是「第 k 个删行 ↔ 第 k 个增行」硬配 —— 那会把毫不相干的两行
            // 凑成一处「修改」（实测 `author: cxk` 配给 `new line here`，行内高亮全乱）。
            // 改成在这块的「删行 × 增行」里贪心挑相似度最高的组合，低于 kMinPairScore 的
            // 一律不配，老老实实报成仅左 / 仅右。
            const bool useSimilarity =
                qint64(removed.size()) * qint64(added.size()) <= detail::kMaxPairCandidates;
            QVector<bool> removedUsed(removed.size(), false);
            QVector<bool> addedUsed(added.size(), false);
            QVector<QPair<int, int>> matched;   // (removed 下标, added 下标)

            for (int r = 0; r < removed.size(); ++r) {
                const QString &leftText = raw.at(removed.at(r)).leftText;
                int best = -1;
                double bestScore = detail::kMinPairScore;
                for (int c = 0; c < added.size(); ++c) {
                    if (addedUsed.at(c))
                        continue;
                    if (!useSimilarity) {
                        // 超大块：退化成按顺序配对，只保证代价可控
                        best = c;
                        break;
                    }
                    const double score =
                        detail::similarity(leftText, raw.at(added.at(c)).rightText);
                    if (score >= bestScore) {
                        bestScore = score;
                        best = c;
                    }
                }
                if (best < 0)
                    continue;
                addedUsed[best] = true;
                removedUsed[r] = true;
                matched.append(qMakePair(r, best));
            }

            for (const QPair<int, int> &pair : matched) {
                Row merged = raw.at(removed.at(pair.first));
                merged.kind = Kind::Changed;
                merged.rightLine = raw.at(added.at(pair.second)).rightLine;
                merged.rightText = raw.at(added.at(pair.second)).rightText;
                detail::markInline(&merged);
                ++result.changedLines;
                result.rows.append(merged);
            }
            for (int r = 0; r < removed.size(); ++r) {
                if (removedUsed.at(r))
                    continue;
                ++result.removedLines;
                result.rows.append(raw.at(removed.at(r)));
            }
            for (int c = 0; c < added.size(); ++c) {
                if (addedUsed.at(c))
                    continue;
                ++result.addedLines;
                result.rows.append(raw.at(added.at(c)));
            }
        } else {
            // 一侧完全为空：整块都是单侧行
            for (int k = 0; k < removed.size(); ++k) {
                ++result.removedLines;
                result.rows.append(raw.at(removed.at(k)));
            }
            for (int k = 0; k < added.size(); ++k) {
                ++result.addedLines;
                result.rows.append(raw.at(added.at(k)));
            }
        }
    }
    return result;
}

} // namespace textdiff

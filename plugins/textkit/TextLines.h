#pragma once

// ---------------------------------------------------------------------------
// 纯文本行处理（只依赖 QtCore，可单独 include 做断言测试）。
//
// 这里放「加引号拼接」「集合/多重集差」两件事，刻意不碰任何
// QWidget：面板只负责取文本、显示结果，算法全在这里，规则 2 要求的临时测试才能
// 直接 include 本文件验证，而不是复制粘贴一份逻辑去测（参考 ExprEval.h 的做法）。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace textlines {

///
/// 按行切分文本。
///
/// 三种行尾都认（`\r\n` / `\n` / `\r`）——从 Windows 记事本、浏览器、Excel 复制
/// 出来的内容行尾并不统一，只认 `\n` 会在 `\r` 上留下看不见的尾巴，导致
/// 「两段看起来一样」的文字被判定为不同。
///
/// 末尾的换行符**不产生**额外的空行：`"a\nb\n"` 是两行而不是三行，这与编辑器里
/// 看到的行数一致，也避免每个粘贴的文本都凭空多出一个「仅在左侧」的空行差异。
inline QStringList splitLines(const QString &text)
{
    QStringList lines;
    if (text.isEmpty())
        return lines;

    const int n = text.size();
    int start = 0;
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('\n') && c != QLatin1Char('\r'))
            continue;
        lines.append(text.mid(start, i - start));
        // `\r\n` 是一个换行：吃掉的 `\r` 后面那个 `\n` 要一起跳过，否则会多切一行。
        if (c == QLatin1Char('\r') && i + 1 < n && text.at(i + 1) == QLatin1Char('\n'))
            ++i;
        start = i + 1;
    }
    // start == n 说明文本以换行结尾，最后那段空串不当作一行。
    if (start < n)
        lines.append(text.mid(start));
    return lines;
}

/// 加引号拼接的选项。默认值对应最常见的用途：`1\n2\n3` → `'1','2','3'`。
struct QuoteOptions
{
    QChar   leftQuote;                 ///< 左引号；QChar() 视为「不加引号」
    QChar   rightQuote;                ///< 右引号
    QString separator = QStringLiteral(",");  ///< 行间分隔串
    bool    trimSpaces = true;         ///< 去掉每行首尾空白（粘贴常带缩进）
    bool    dropEmpty = true;          ///< 丢弃空行（只有空白的行也算空行）
    bool    unique = false;            ///< 去重，保留首次出现的那一个
};

///
/// 把多行文本按选项加工成一组条目：逐行 trim → 丢空行 → 去重 → 套引号。
///
/// 空行要不要套引号？dropEmpty 关掉时照样套成 `''`：那通常意味着「这是一个空字段」，
/// 去掉引号会让 SQL IN 列表 / CSV 的项数对不上。
inline QStringList quoteItems(const QString &text, const QuoteOptions &opt)
{
    const QStringList raw = splitLines(text);
    QStringList out;
    out.reserve(raw.size());
    QSet<QString> seen;

    const bool hasQuote = !opt.leftQuote.isNull() || !opt.rightQuote.isNull();
    for (const QString &line : raw) {
        const QString item = opt.trimSpaces ? line.trimmed() : line;
        if (opt.dropEmpty && item.trimmed().isEmpty())
            continue;
        if (opt.unique) {
            if (seen.contains(item))
                continue;
            seen.insert(item);
        }
        if (hasQuote)
            out.append(QString(opt.leftQuote) + item + QString(opt.rightQuote));
        else
            out.append(item);
    }
    return out;
}

/// 把 quoteItems() 的结果用分隔串连成一行。条目数由调用方从 quoteItems() 拿。
inline QString quoteJoin(const QString &text, const QuoteOptions &opt)
{
    return quoteItems(text, opt).join(opt.separator);
}

/// 两段的差异结果。三个列表都保持「首次出现顺序」，方便和原文对照。
struct SetDiff
{
    QStringList onlyLeft;    ///< 只在左侧出现（右侧没有，或出现次数更少）
    QStringList onlyRight;   ///< 只在右侧出现
    QStringList common;      ///< 两侧都有
    int leftItems = 0;       ///< 左侧有效行数（已按 ignoreEmpty 过滤）
    int rightItems = 0;      ///< 右侧有效行数
};

///
/// 比较两段文本里「各自独有的行」。
///
/// 语义由 byCount 决定：
///   * false（默认）—— 集合语义：同一个值只列一次，适合「找两批编号/ID 的差集」；
///   * true  —— 多重集语义：按出现次数补齐差额，适合「两版日志比少了哪几条」。
///
/// 行比较前做 trim：粘贴来源不同常带行尾空格，若不 trim 会把「`3`」和「`3 `」
/// 当成两个不同的值，出现「明明两边都有却报只在左边」的怪结果。
inline SetDiff compareSets(const QString &leftText, const QString &rightText,
                           bool ignoreEmpty, bool byCount)
{
    SetDiff result;

    QHash<QString, int> countLeft;
    QHash<QString, int> countRight;
    QStringList orderLeft;    // 首次出现顺序：结果列表按它排，和用户读原文的顺序一致
    QStringList orderRight;

    const auto collect = [ignoreEmpty](const QString &text, QHash<QString, int> *counts,
                                       QStringList *order, int *items) {
        const QStringList lines = splitLines(text);
        for (const QString &raw : lines) {
            const QString item = raw.trimmed();
            if (ignoreEmpty && item.isEmpty())
                continue;
            if (!counts->contains(item))
                order->append(item);
            ++(*counts)[item];
            ++(*items);
        }
    };
    collect(leftText, &countLeft, &orderLeft, &result.leftItems);
    collect(rightText, &countRight, &orderRight, &result.rightItems);

    // 集合语义下「出现次数不同」不算差异（左侧两个 a、右侧一个 a 视为一致），
    // 只有右侧完全没有这个值才进 onlyLeft；多出/缺少的重复次数交给 byCount。
    for (const QString &item : orderLeft) {
        const int inRight = countRight.value(item);
        if (inRight <= 0) {
            const int times = byCount ? countLeft.value(item) : 1;
            for (int i = 0; i < times; ++i)
                result.onlyLeft.append(item);
            continue;
        }
        if (!byCount)
            continue;
        const int extra = countLeft.value(item) - inRight;
        for (int i = 0; i < extra; ++i)
            result.onlyLeft.append(item);
    }
    for (const QString &item : orderRight) {
        const int inLeft = countLeft.value(item);
        if (inLeft <= 0) {
            const int times = byCount ? countRight.value(item) : 1;
            for (int i = 0; i < times; ++i)
                result.onlyRight.append(item);
            continue;
        }
        if (!byCount)
            continue;
        const int extra = countRight.value(item) - inLeft;
        for (int i = 0; i < extra; ++i)
            result.onlyRight.append(item);
    }
    for (const QString &item : orderLeft) {
        const int inRight = countRight.value(item);
        if (inRight <= 0)
            continue;
        const int times = byCount ? qMin(countLeft.value(item), inRight) : 1;
        for (int i = 0; i < times; ++i)
            result.common.append(item);
    }
    return result;
}

} // namespace textlines

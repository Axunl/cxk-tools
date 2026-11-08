#pragma once

// ---------------------------------------------------------------------------
// JSON / 字符串转义工具（纯逻辑，无 UI 依赖，可脱离界面单独验证）。
//
// 放在头文件里而不是匿名命名空间：这样临时测试工程可以直接 include 验证，
// 不用把逻辑复制一份（复制一份等于没测）。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QJsonParseError>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace jsontext {

/// 字符偏移 → 0 起始的行号（QTextDocument 的块号与之一致）。
///
/// QJsonParseError 只给 offset，用户面对大段 JSON 时这个数字毫无意义，
/// 必须换算成行列才好定位，也才能把出错行标出来。
inline int lineOfOffset(const QString &text, int offset)
{
    // qBound 防御 offset 越界（极端情况下 err.offset 可能等于 size）
    const int end = qBound(0, offset, static_cast<int>(text.size()));
    int line = 0;
    for (int i = 0; i < end; ++i) {
        if (text.at(i) == QLatin1Char('\n'))
            ++line;
    }
    return line;
}

/// 把 QJsonParseError 的字符偏移翻译成"第几行第几列"（行列都从 1 开始给人看）。
inline QString describeParseError(const QJsonParseError &err, const QString &text)
{
    const int offset = qBound(0, err.offset, static_cast<int>(text.size()));
    const int line = lineOfOffset(text, offset);
    // lastIndexOf 的 from 参数是"从这里往前找"，offset-1 是为了不含当前字符本身
    const int lineStart = text.lastIndexOf(QLatin1Char('\n'), offset - 1) + 1;
    return QStringLiteral("%1（第 %2 行 第 %3 列）")
        .arg(err.errorString())
        .arg(line + 1)
        .arg(offset - lineStart + 1);
}

/// 从开引号下标找到闭引号后一位；越过行尾仍未闭合则返回 -1。
///
/// JSON 字符串不允许跨行，所以遇到换行就可以判定"未闭合"——
/// 高亮器据此把残缺字符串标成错误色，补全逻辑据此停止向下扫描。
inline int stringEnd(const QString &text, int quotePos)
{
    const int size = text.size();
    for (int i = quotePos + 1; i < size; ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('\n'))
            return -1;
        // 反斜杠吃掉下一个字符：`\"` 不是边界，`\\` 之后才是
        if (ch == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (ch == QLatin1Char('"'))
            return i + 1;
    }
    return -1;
}

/// 收集文本里出现的所有 JSON 键（按首次出现顺序、已去重）。
///
/// 刻意只做词法扫描、不要求整段合法：用户正在敲的半截内容也要能给出候选，
/// 所以判定依据是"字符串后面跟着冒号"，而不是真的解析成对象。
/// 超大文本直接放弃（补全候选不值当扫几 MB）；候选也有上限 —— 几千个键的弹窗
/// 既找不到也没法用，还拖慢每次按键的重建。
inline QStringList collectKeys(const QString &text)
{
    constexpr int kMaxKeys = 400;

    QStringList keys;
    if (text.size() > 4 * 1024 * 1024)
        return keys;

    QSet<QString> seen;
    const int size = text.size();
    for (int i = 0; i < size; ++i) {
        if (text.at(i) != QLatin1Char('"'))
            continue;

        const int end = stringEnd(text, i);
        if (end < 0) {
            // 本行字符串没闭合：跳到下一行继续，否则一个手误就让后面全丢
            const int nextLine = text.indexOf(QLatin1Char('\n'), i);
            if (nextLine < 0)
                break;
            i = nextLine;
            continue;
        }

        int j = end;
        while (j < size && text.at(j).isSpace())
            ++j;
        if (j < size && text.at(j) == QLatin1Char(':')) {
            const QString key = text.mid(i + 1, end - i - 2);
            if (!key.isEmpty() && !seen.contains(key)) {
                seen.insert(key);
                keys.append(key);
                if (keys.size() >= kMaxKeys)
                    break;
            }
        }
        i = end - 1;
    }
    return keys;
}

/// 光标是否落在未闭合的 JSON 字符串里（只扫当前行：字符串不跨行）。
///
/// 用途是让引号自动配对知道"这一下是开引号还是收引号"——
/// 在字符串里再按引号必须原样插入，否则会插出一对多余的引号。
inline bool insideString(const QString &text, int pos)
{
    const int end = qBound(0, pos, static_cast<int>(text.size()));
    int lineStart = 0;
    for (int i = end - 1; i >= 0; --i) {
        if (text.at(i) == QLatin1Char('\n')) {
            lineStart = i + 1;
            break;
        }
    }

    int quotes = 0;
    for (int i = lineStart; i < end; ++i) {
        if (text.at(i) == QLatin1Char('\\')) {   // 跳过被转义的引号
            ++i;
            continue;
        }
        if (text.at(i) == QLatin1Char('"'))
            ++quotes;
    }
    return quotes % 2 == 1;
}

/// 光标所在字符串的开引号下标；不在字符串内返回 -1。
inline int stringStartAt(const QString &text, int pos)
{
    const int end = qBound(0, pos, static_cast<int>(text.size()));
    int lineStart = 0;
    for (int i = end - 1; i >= 0; --i) {
        if (text.at(i) == QLatin1Char('\n')) {
            lineStart = i + 1;
            break;
        }
    }

    int open = -1;
    bool inString = false;
    for (int i = lineStart; i < end; ++i) {
        const QChar ch = text.at(i);
        if (inString && ch == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (ch != QLatin1Char('"'))
            continue;
        if (inString) {
            inString = false;
            open = -1;
        } else {
            inString = true;
            open = i;
        }
    }
    return inString ? open : -1;
}

/// 开引号是否处在"键的位置"（即这个字符串是键，不是值）。
///
/// 判据是往前看第一个非空白字符：`{` 或 `,` 之后是键，`:` 之后是值。
/// 已知近似：数组里的字符串也会被判成键位置 —— 准确区分要维护括号栈、
/// 每次按键都从头扫一遍代价太大，而多弹几个候选最多只是噪音。
inline bool inKeyPosition(const QString &text, int stringStart)
{
    int i = stringStart - 1;
    while (i >= 0 && text.at(i).isSpace())
        --i;
    if (i < 0)
        return true;   // 文首的字符串，按键处理
    const QChar prev = text.at(i);
    return prev == QLatin1Char('{') || prev == QLatin1Char(',');
}

/// 光标前"待补全片段"的起点（光标在片段末尾）。
///
/// 片段只认字母/数字/下划线/连字符/点：引号、冒号、空白都是天然边界，
/// 这样中文键名（QChar::isLetterOrNumber 为真）也能正常吃到前缀。
inline int completionPrefixStart(const QString &text, int pos)
{
    int i = qBound(0, pos, static_cast<int>(text.size()));
    while (i > 0) {
        const QChar ch = text.at(i - 1);
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('_') && ch != QLatin1Char('-')
            && ch != QLatin1Char('.')) {
            break;
        }
        --i;
    }
    return i;
}

/// 一个"能折起来"的区块：开括号所在行 + 与之配对的闭括号所在行（行号 0 起始，
/// 与 QTextDocument 的块号一致）。
struct FoldRegion
{
    int startLine = -1;
    int endLine = -1;
};

/// 括号是否成对（`{` 只配 `}`、`[` 只配 `]`）。
inline bool bracketsMatch(QChar open, QChar close)
{
    return (open == QLatin1Char('{') && close == QLatin1Char('}'))
        || (open == QLatin1Char('[') && close == QLatin1Char(']'));
}

/// 找出所有**跨行**的 `{}` / `[]` 区块（折叠用）。
///
/// 判定刻意宽松：这是给"读"用的能力，不是校验器。括号没配齐（用户正在敲）、
/// 类型错配（`[}`）都不报错，只是那一块不产生折叠区 —— 宁可少一个箭头，
/// 也不要因为一个手误让整份文档的折叠全部失效。
/// 单行区块（`{"a": 1}`）不返回：折起来只会把这一行自己藏掉，没有意义。
inline QVector<FoldRegion> foldRegions(const QStringList &lines)
{
    struct Open
    {
        int line;
        QChar ch;
    };

    QVector<Open> stack;
    QVector<FoldRegion> regions;

    for (int line = 0; line < lines.size(); ++line) {
        const QString &text = lines.at(line);
        for (int i = 0; i < text.size(); ++i) {
            const QChar ch = text.at(i);
            if (ch == QLatin1Char('"')) {
                // 字符串里的括号不是结构，整段跳过去；本行没闭合就到此为止
                const int end = stringEnd(text, i);
                if (end < 0)
                    break;
                i = end - 1;
                continue;
            }
            if (ch != QLatin1Char('{') && ch != QLatin1Char('[') && ch != QLatin1Char('}')
                && ch != QLatin1Char(']')) {
                continue;
            }
            if (ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
                stack.append({line, ch});
                continue;
            }

            // 闭括号：从栈顶往下找同类型的开括号
            int match = -1;
            for (int s = stack.size() - 1; s >= 0; --s) {
                if (bracketsMatch(stack.at(s).ch, ch)) {
                    match = s;
                    break;
                }
            }
            if (match < 0)
                continue;   // 多余的闭括号：忽略，不影响其它区块
            const int openLine = stack.at(match).line;
            // 中间那些配不上的开括号一并丢掉，否则它们永远留在栈里挡住后面的配对
            stack.resize(match);
            if (line > openLine)
                regions.append({openLine, line});
        }
    }
    return regions;
}

/// 转义：把任意文本变成合法的 JSON 字符串内容。
inline QString escapeText(const QString &text)
{
    QString out;
    out.reserve(text.size() + 16);

    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case '\\': out += QStringLiteral("\\\\"); break;
        case '"':  out += QStringLiteral("\\\""); break;
        case '\n': out += QStringLiteral("\\n"); break;
        case '\r': out += QStringLiteral("\\r"); break;
        case '\t': out += QStringLiteral("\\t"); break;
        default:
            if (ch.unicode() < 0x20) {
                // 控制字符没有可读形式，只能写成 \u00XX。
                // 注意 QChar::unicode() 返回 char16_t，而 QString::arg 的
                // (值, 宽度, 进制, 填充) 重载只接受整型，这里显式转 uint。
                out += QStringLiteral("\\u%1").arg(static_cast<uint>(ch.unicode()), 4, 16, QLatin1Char('0'));
            } else {
                out += ch;
            }
        }
    }
    return out;
}

/// 转义的逆操作。无法识别的转义序列原样保留（宁可保留也别吃掉内容）。
inline QString unescapeText(const QString &text)
{
    QString out;
    out.reserve(text.size());

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch != QLatin1Char('\\') || i + 1 >= text.size()) {
            out += ch;
            continue;
        }

        const QChar next = text.at(++i);
        switch (next.unicode()) {
        case 'n':  out += QLatin1Char('\n'); break;
        case 'r':  out += QLatin1Char('\r'); break;
        case 't':  out += QLatin1Char('\t'); break;
        case 'b':  out += QLatin1Char('\b'); break;
        case 'f':  out += QLatin1Char('\f'); break;
        case '"':  out += QLatin1Char('"'); break;
        case '\'': out += QLatin1Char('\''); break;
        case '\\': out += QLatin1Char('\\'); break;
        case '/':  out += QLatin1Char('/'); break;
        case 'u': {
            // 必须恰好 4 位十六进制：少了（文本被截断）不算合法转义，
            // 否则 "\u12" 会被当成 0x12 输出一个控制字符，把用户文本吃掉。
            const QString hex = text.mid(i + 1, 4);
            bool ok = false;
            uint code = 0;
            if (hex.size() == 4)
                code = hex.toUInt(&ok, 16);
            if (ok) {
                out += QChar(static_cast<char16_t>(code));
                i += 4;
            } else {
                out += QLatin1Char('\\');
                out += next;
            }
            break;
        }
        default:
            out += QLatin1Char('\\');
            out += next;
        }
    }
    return out;
}

} // namespace jsontext

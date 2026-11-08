#pragma once

// ---------------------------------------------------------------------------
// JSON / 字符串转义工具（纯逻辑，无 UI 依赖，可脱离界面单独验证）。
//
// 放在头文件里而不是匿名命名空间：这样临时测试工程可以直接 include 验证，
// 不用把逻辑复制一份（复制一份等于没测）。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QJsonParseError>
#include <QString>

namespace jsontext {

/// 把 QJsonParseError 的字符偏移翻译成"第几行第几列"。
///
/// QJsonDocument 只给 offset，用户面对大段 JSON 时这个数字毫无意义，
/// 必须换算成行列才好定位。
inline QString describeParseError(const QJsonParseError &err, const QString &text)
{
    int line = 1;
    int column = 1;
    // qBound 防御 offset 越界（极端情况下 err.offset 可能等于 size）
    const int offset = qBound(0, err.offset, static_cast<int>(text.size()));
    for (int i = 0; i < offset; ++i) {
        if (text.at(i) == QLatin1Char('\n')) {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return QStringLiteral("%1（第 %2 行 第 %3 列）").arg(err.errorString()).arg(line).arg(column);
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

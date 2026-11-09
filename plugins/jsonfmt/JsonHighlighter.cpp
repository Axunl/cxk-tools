#include "JsonHighlighter.h"

#include "JsonText.h"

#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用）   // jsontext::stringEnd

#include <QColor>
#include <QFont>
#include <QTextDocument>

namespace {

/// JSON 只认 ASCII 数字。QChar::isDigit() 对阿拉伯-印度数字也为真，
/// 用它会把非法字符当成数字染成正常色，反而掩盖错误。
bool isAsciiDigit(QChar ch)
{
    return ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
}

/// 关键词后面必须断开（"true" 不能匹配 "trueX"，也不能吃掉 "truevalue"）。
bool isWordBoundary(const QString &text, int index)
{
    if (index >= text.size())
        return true;
    const QChar ch = text.at(index);
    return !ch.isLetterOrNumber() && ch != QLatin1Char('_');
}

/// 扫描一个 JSON 数字，返回结束下标（不含）。语法：-?(int)(.frac)?([eE][+-]?exp)?
///
/// 这里不追求严格校验（`1.2.3` 会分成两段数字），高亮的职责是让人看清结构，
/// 真正的合法性由格式化按钮的 QJsonDocument 判定。
int scanNumber(const QString &text, int start)
{
    const int size = text.size();
    int i = start;
    if (i < size && text.at(i) == QLatin1Char('-'))
        ++i;

    while (i < size && isAsciiDigit(text.at(i)))
        ++i;

    if (i < size && text.at(i) == QLatin1Char('.')) {
        ++i;
        while (i < size && isAsciiDigit(text.at(i)))
            ++i;
    }

    if (i < size && (text.at(i) == QLatin1Char('e') || text.at(i) == QLatin1Char('E'))) {
        int j = i + 1;
        if (j < size && (text.at(j) == QLatin1Char('+') || text.at(j) == QLatin1Char('-')))
            ++j;
        if (j < size && isAsciiDigit(text.at(j))) {
            i = j;
            while (i < size && isAsciiDigit(text.at(i)))
                ++i;
        }
    }
    return i;
}

} // namespace

JsonHighlighter::JsonHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    // 配色取自主题令牌（浅/深各一套）：键用强调蓝、字符串用绿、数字用琥珀、
    // 字面量用紫、标点压成次要灰，保证扫一眼就能分出结构层次。
    // 注意：格式对象在构造时按当时的主题取值——面板每次打开都会新建编辑器，
    // 所以换肤后重新进入工具即可得到新配色。
    m_key.setForeground(xtools::theme::color(xtools::theme::Color::SyntaxKey));
    m_key.setFontWeight(QFont::DemiBold);

    m_string.setForeground(xtools::theme::color(xtools::theme::Color::SyntaxString));
    m_number.setForeground(xtools::theme::color(xtools::theme::Color::SyntaxNumber));
    m_literal.setForeground(xtools::theme::color(xtools::theme::Color::SyntaxLiteral));
    m_punct.setForeground(xtools::theme::color(xtools::theme::Color::SyntaxPunct));

    // 未闭合字符串：波浪下划线比纯变色更明确地表达"这里有问题"
    m_error.setForeground(xtools::theme::color(xtools::theme::Color::SyntaxError));
    m_error.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    m_error.setUnderlineColor(xtools::theme::color(xtools::theme::Color::SyntaxError));
}

void JsonHighlighter::highlightBlock(const QString &text)
{
    const int size = text.size();
    int i = 0;
    while (i < size) {
        const QChar ch = text.at(i);

        // ---- 字符串：先整段吃掉，再回头看闭引号后面是不是冒号来判键/值
        if (ch == QLatin1Char('"')) {
            const int end = jsontext::stringEnd(text, i);
            if (end < 0) {
                // 行内没有闭引号 = 语法错误，整段标红并结束本行
                setFormat(i, size - i, m_error);
                return;
            }

            int j = end;
            while (j < size && text.at(j).isSpace())
                ++j;
            const bool isKey = j < size && text.at(j) == QLatin1Char(':');
            setFormat(i, end - i, isKey ? m_key : m_string);
            i = end;
            continue;
        }

        // ---- 数字：'-' 与数字都算入口，避免把负号漏成标点色
        if (ch == QLatin1Char('-') || isAsciiDigit(ch)) {
            const int end = scanNumber(text, i);
            setFormat(i, end - i, m_number);
            i = end;
            continue;
        }

        // ---- 字面量
        if (ch == QLatin1Char('t') && text.mid(i, 4) == QLatin1String("true")
            && isWordBoundary(text, i + 4)) {
            setFormat(i, 4, m_literal);
            i += 4;
            continue;
        }
        if (ch == QLatin1Char('f') && text.mid(i, 5) == QLatin1String("false")
            && isWordBoundary(text, i + 5)) {
            setFormat(i, 5, m_literal);
            i += 5;
            continue;
        }
        if (ch == QLatin1Char('n') && text.mid(i, 4) == QLatin1String("null")
            && isWordBoundary(text, i + 4)) {
            setFormat(i, 4, m_literal);
            i += 4;
            continue;
        }

        // ---- 结构标点
        if (ch == QLatin1Char('{') || ch == QLatin1Char('}') || ch == QLatin1Char('[')
            || ch == QLatin1Char(']') || ch == QLatin1Char(',') || ch == QLatin1Char(':')) {
            setFormat(i, 1, m_punct);
        }
        ++i;
    }
}

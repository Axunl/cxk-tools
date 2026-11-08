#pragma once

// ---------------------------------------------------------------------------
// 表达式求值器（纯逻辑，无 Qt Widgets 依赖，可脱离界面单独验证）。
//
// 文法（优先级从低到高）：
//   expr    := term (('+'|'-') term)*
//   term    := unary (('*'|'/'|'%') unary)*
//   unary   := ('-'|'+') unary | power
//   power   := primary ('^' unary)?          // 右结合：2^3^2 = 2^9
//   primary := number | '(' expr ')' | 'pi' | 'e'
//
// 为什么手写而不是用 QJSEngine：后者要拖进整个 JS 引擎（体积 + 启动开销），
// 而这里只需要四则运算 + 幂；手写不到 200 行，行为完全可控。
//
// 错误处理：失败写进 error 输出参数并返回 false（项目不用异常）。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QString>

#include <cmath>

namespace calc {

/// 常量表。用 double 直接写字面量而不是 M_PI：后者依赖 <cmath> 的非标准扩展，
/// MSVC 需要 _USE_MATH_DEFINES，跨平台会踩坑。
constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 2.71828182845904523536;

class ExprEval
{
public:
    explicit ExprEval(const QString &text)
        : m_text(text)
    {
    }

    /// 求值。
    /// @param result 成功时写入结果（可为 nullptr）
    /// @param error 失败时写入中文原因（可为 nullptr）
    bool evaluate(double *result, QString *error)
    {
        m_pos = 0;
        m_error.clear();

        const double value = parseExpr();

        // 解析完若还有剩余字符，说明存在无法识别的片段（例如 "1 2"）
        skipSpaces();
        if (m_error.isEmpty() && m_pos < m_text.size())
            m_error = QStringLiteral("无法识别：%1").arg(m_text.mid(m_pos, 6));

        if (!m_error.isEmpty()) {
            if (error)
                *error = m_error;
            return false;
        }

        // 溢出/NaN 都当失败：界面上显示 "inf" 对用户没有意义
        if (!std::isfinite(value)) {
            if (error)
                *error = QStringLiteral("结果不是有限数");
            return false;
        }

        if (result)
            *result = value;
        return true;
    }

private:
    void skipSpaces()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }

    /// 看下一个非空白字符，不移动游标。
    QChar peek()
    {
        skipSpaces();
        return m_pos < m_text.size() ? m_text.at(m_pos) : QChar();
    }

    double parseExpr()
    {
        double value = parseTerm();
        while (m_error.isEmpty()) {
            const QChar c = peek();
            if (c == QLatin1Char('+')) {
                ++m_pos;
                value += parseTerm();
            } else if (c == QLatin1Char('-')) {
                ++m_pos;
                value -= parseTerm();
            } else {
                break;
            }
        }
        return value;
    }

    double parseTerm()
    {
        double value = parseUnary();
        while (m_error.isEmpty()) {
            const QChar c = peek();
            if (c == QLatin1Char('*')) {
                ++m_pos;
                value *= parseUnary();
            } else if (c == QLatin1Char('/')) {
                ++m_pos;
                const double divisor = parseUnary();
                // 除零报错而不是给 inf：用户按 1/0 时期待的是提示
                if (m_error.isEmpty() && divisor == 0.0) {
                    m_error = QStringLiteral("除数不能为 0");
                    return 0;
                }
                value /= divisor;
            } else if (c == QLatin1Char('%')) {
                ++m_pos;
                const double divisor = parseUnary();
                if (m_error.isEmpty() && divisor == 0.0) {
                    m_error = QStringLiteral("取余的除数不能为 0");
                    return 0;
                }
                value = std::fmod(value, divisor);
            } else {
                break;
            }
        }
        return value;
    }

    double parseUnary()
    {
        const QChar c = peek();
        if (c == QLatin1Char('-')) {
            ++m_pos;
            return -parseUnary();
        }
        if (c == QLatin1Char('+')) {
            ++m_pos;
            return parseUnary();
        }
        return parsePower();
    }

    double parsePower()
    {
        const double base = parsePrimary();
        if (m_error.isEmpty() && peek() == QLatin1Char('^')) {
            ++m_pos;
            // 递归 parseUnary 而不是 parsePower，得到右结合：2^3^2 = 2^(3^2)
            const double exponent = parseUnary();
            return std::pow(base, exponent);
        }
        return base;
    }

    double parsePrimary()
    {
        const QChar c = peek();

        if (c == QLatin1Char('(')) {
            ++m_pos;
            const double value = parseExpr();
            if (m_error.isEmpty()) {
                if (peek() == QLatin1Char(')'))
                    ++m_pos;
                else
                    m_error = QStringLiteral("缺少右括号");
            }
            return value;
        }

        // 常量：连续字母组成名字，只认 pi / e
        if (c.isLetter()) {
            const int start = m_pos;
            while (m_pos < m_text.size() && m_text.at(m_pos).isLetter())
                ++m_pos;
            const QString name = m_text.mid(start, m_pos - start).toLower();
            if (name == QStringLiteral("pi"))
                return kPi;
            if (name == QStringLiteral("e"))
                return kE;
            m_error = QStringLiteral("未知名称：%1").arg(name);
            return 0;
        }

        // 数字：允许小数点，但不做科学计数法（"1e3" 会与常量 e 冲突）
        const int start = m_pos;
        while (m_pos < m_text.size()
               && (m_text.at(m_pos).isDigit() || m_text.at(m_pos) == QLatin1Char('.')))
            ++m_pos;

        if (start == m_pos) {
            m_error = c.isNull() ? QStringLiteral("表达式不完整")
                                 : QStringLiteral("这里需要一个数字，却遇到 '%1'").arg(c);
            return 0;
        }

        bool ok = false;
        const double value = m_text.mid(start, m_pos - start).toDouble(&ok);
        if (!ok) {
            m_error = QStringLiteral("数字格式错误：%1").arg(m_text.mid(start, m_pos - start));
            return 0;
        }
        return value;
    }

    QString m_text;
    int m_pos = 0;
    QString m_error;
};

/// 结果格式化：整数不带小数点（3.0 -> "3"），其余保留 12 位有效数字。
///
/// 12 位而不是 17 位（double 全精度）：用户按 0.1+0.2 时期待看到 0.3，
/// 而不是 0.30000000000000004。
inline QString formatNumber(double value)
{
    if (std::fabs(value - std::round(value)) < 1e-10 && std::fabs(value) < 1e15)
        return QString::number(static_cast<qlonglong>(std::llround(value)));
    return QString::number(value, 'g', 12);
}

} // namespace calc

#pragma once

// ---------------------------------------------------------------------------
// 取色器的纯逻辑层：颜色文本解析、多格式输出、对比度与建议文字色。
//
// 刻意不依赖 QWidget / 剪贴板 / 屏幕：临时验证工程可以直接 include 本文件做断言
// （AGENTS.md 规则 2 的落地方式，与 plugins/calc/ExprEval.h 同一形态）。
// 全部函数 inline，头文件被多个翻译单元包含不会产生重复定义。
// ---------------------------------------------------------------------------

#include <QColor>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cmath>

namespace colortext {

/// 一行「格式名 + 可复制文本」。面板只负责把它摆进表格，内容在这里集中生成。
struct Field
{
    QString label;
    QString text;
};

namespace detail {

/// 函数式颜色（rgb()/hsl() ...）里的一个数字；percent 记录是否带 % 后缀。
struct Number
{
    qreal value = 0;
    bool percent = false;
};

/// 逗号、斜杠、空白都当分隔符（CSS Color 4 允许混用，设计稿里也常见空格写法）。
/// 任意一个数字解析失败就返回空列表，整串判为非法 —— 不做「半解析」，
/// 否则 "rgb(1, x, 3)" 会被当成某种奇怪的默认颜色，用户反而看不出输错了。
inline QList<Number> splitNumbers(const QString &body)
{
    static const QRegularExpression sep(QStringLiteral("[\\s,/]+"));
    const QStringList parts = body.split(sep, Qt::SkipEmptyParts);
    QList<Number> out;
    out.reserve(parts.size());
    for (const QString &raw : parts) {
        QString token = raw.trimmed();
        // 角度单位只可能是 deg（hsl 的色相）；turn/rad 少见于取色场景，不支持，
        // 解析不了就走「非法输入」提示，避免悄悄按错误的单位换算。
        if (token.endsWith(QLatin1String("deg"), Qt::CaseInsensitive))
            token.chop(3);
        const bool percent = token.endsWith(QLatin1Char('%'));
        if (percent)
            token.chop(1);
        bool ok = false;
        const qreal value = token.toDouble(&ok);
        if (!ok)
            return {};
        out.append(Number{value, percent});
    }
    return out;
}

/// 通道归一到 0..1：带 % 就除以 100，否则按 scale（RGB 用 255、色相用 360）除。
inline qreal channel(const Number &n, qreal scale)
{
    return qBound<qreal>(0, n.percent ? n.value / 100.0 : n.value / scale, 1);
}

/// alpha 单独处理：不带 % 时 CSS 里是 0..1 的小数，不是 0..255。
inline qreal alphaOf(const Number &n)
{
    return qBound<qreal>(0, n.percent ? n.value / 100.0 : n.value, 1);
}

/// 把解析结果统一成 8 位整数 RGBA 规格。
///
/// 为什么必须做这一步：QColor::fromRgbF(..., 0.5) 把 alpha 存成 16 位定点的
/// 0.5*65535，而 QColor(r, g, b, 128) 存的是 128/255 —— 两者 alpha() 都显示
/// 128，QColor::operator== 却判不等。历史色块去重、面板里比较「同色」都会因此
/// 失效。统一规格后，屏幕像素 / 函数式输入 / 命名色三种来源可以安全比较。
inline QColor normalized(const QColor &c)
{
    return c.isValid() ? QColor::fromRgb(c.red(), c.green(), c.blue(), c.alpha()) : QColor();
}

/// 解析 #RGB / #RGBA / #RRGGBB / #RRGGBBAA 或 0xRRGGBB 形式的十六进制颜色。
/// 返回 false 表示不是一个合法的十六进制颜色串。
inline bool parseHexString(const QString &text, QColor *out)
{
    QString hex = text;
    if (hex.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        hex = hex.mid(2);
    else if (hex.startsWith(QLatin1Char('#')))
        hex = hex.mid(1);
    else
        return false;

    // #RGB / #RGBA 按 CSS 规则逐位展开（#abc == #aabbcc）。
    if (hex.size() == 3 || hex.size() == 4) {
        QString expanded;
        expanded.reserve(hex.size() * 2);
        for (QChar c : hex) {
            expanded.append(c);
            expanded.append(c);
        }
        hex = expanded;
    }
    if (hex.size() != 6 && hex.size() != 8)
        return false;

    int rgba[4] = {0, 0, 0, 255};
    for (int i = 0; i < hex.size() / 2; ++i) {
        bool ok = false;
        const int value = hex.mid(i * 2, 2).toInt(&ok, 16);
        if (!ok)
            return false;
        rgba[i] = value;
    }

    // 8 位按 CSS 的 #RRGGBBAA 解释。这里不走 QColor::setNamedColor()：
    // Qt 把 #AARRGGBB 的前两位当 alpha，与 CSS 正好相反，手动解析才能消除歧义。
    *out = QColor::fromRgb(rgba[0], rgba[1], rgba[2], hex.size() == 8 ? rgba[3] : 255);
    return out->isValid();
}

} // namespace detail

/// 把用户输入解析成颜色；失败的返回 QColor()（isValid() == false）。
///
/// 支持：#RGB/#RRGGBB/#RRGGBBAA/#AARRGGBB 之外的 0xRRGGBB、rgb()/rgba()、
/// hsl()/hsla()、hsv()/hsva()、裸数字组「59, 130, 246」以及 Qt 内置的 SVG 颜色名。
/// 所有结果统一转成整数 RGBA 规格（detail::normalized()），保证后面几种格式
/// 都从同一份数据换算，历史去重时也不会因为浮点/整数规格不同判成两个颜色。
inline QColor parseColor(const QString &input)
{
    const QString text = input.trimmed();
    if (text.isEmpty())
        return QColor();

    // 十六进制串只在这里判一次；写错就明确报错，不要再去猜它是不是颜色名。
    if (text.startsWith(QLatin1Char('#')) || text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        QColor hex;
        return detail::parseHexString(text, &hex) ? detail::normalized(hex) : QColor();
    }

    static const QRegularExpression func(QStringLiteral("^([A-Za-z]+)\\(([^()]*)\\)$"));
    const QRegularExpressionMatch match = func.match(text);
    if (match.hasMatch()) {
        const QString fn = match.captured(1).toLower();
        const QList<detail::Number> nums = detail::splitNumbers(match.captured(2));
        const bool rgbLike = (fn == QLatin1String("rgb") || fn == QLatin1String("rgba"));
        const bool hslLike = (fn == QLatin1String("hsl") || fn == QLatin1String("hsla"));
        const bool hsvLike = (fn == QLatin1String("hsv") || fn == QLatin1String("hsva"));
        if ((!rgbLike && !hslLike && !hsvLike) || (nums.size() != 3 && nums.size() != 4))
            return QColor();

        // CSS Color 4 允许 rgb() 也带第 4 个 alpha，这里对三种函数一视同仁。
        const qreal a = nums.size() == 4 ? detail::alphaOf(nums.at(3)) : 1.0;
        QColor color;
        if (rgbLike) {
            color = QColor::fromRgbF(detail::channel(nums.at(0), 255),
                                     detail::channel(nums.at(1), 255),
                                     detail::channel(nums.at(2), 255), a);
        } else if (hslLike) {
            color = QColor::fromHslF(detail::channel(nums.at(0), 360),
                                     detail::channel(nums.at(1), 100),
                                     detail::channel(nums.at(2), 100), a);
        } else {
            color = QColor::fromHsvF(detail::channel(nums.at(0), 360),
                                     detail::channel(nums.at(1), 100),
                                     detail::channel(nums.at(2), 100), a);
        }
        return detail::normalized(color);
    }

    // 裸数字组：设计稿里常直接写「59, 130, 246」或「59 130 246 0.5」。
    // 第 4 个数 >1 时按 0..255 看待，否则按 0..1 的 alpha，兼容两种手写习惯。
    const QList<detail::Number> nums = detail::splitNumbers(text);
    if (nums.size() == 3 || nums.size() == 4) {
        const qreal a = nums.size() == 4
            ? (nums.at(3).value > 1 ? detail::channel(nums.at(3), 255) : detail::alphaOf(nums.at(3)))
            : 1.0;
        return detail::normalized(QColor::fromRgbF(detail::channel(nums.at(0), 255),
                                                   detail::channel(nums.at(1), 255),
                                                   detail::channel(nums.at(2), 255), a));
    }

    // SVG / CSS 命名色交给 Qt 内置的命名色表（跨平台一致，不用自己维护一张表）。
    // Qt 6.5+ 用 QColor::fromString()；setNamedColor() 已被标记 deprecated，
    // 本项目要求构建零警告，不能继续用旧接口。
    return detail::normalized(QColor::fromString(text.toLower()));
}

/// #RRGGBB（大写，和设计稿里的写法一致）。
inline QString formatHex(const QColor &c)
{
    return QStringLiteral("#%1")
        .arg((c.red() << 16) | (c.green() << 8) | c.blue(), 6, 16, QLatin1Char('0'))
        .toUpper();
}

/// #RRGGBBAA：8 位十六进制，alpha 放末尾（与 parseColor 的解析规则对称）。
inline QString formatHexAlpha(const QColor &c)
{
    return formatHex(c) + QStringLiteral("%1").arg(c.alpha(), 2, 16, QLatin1Char('0')).toUpper();
}

inline QString formatRgb(const QColor &c)
{
    return QStringLiteral("rgb(%1, %2, %3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

/// alpha 写成 CSS 常用的 0..1 小数；完全不透明时就写 1，不输出无意义的 1.000。
inline QString formatRgba(const QColor &c)
{
    // 用整数 alpha 判不透明：alphaF() 是 float，与 qreal(double) 混进
    // qFuzzyCompare 会触发重载歧义（qfloat16 / float / double 三个候选）。
    const QString a = c.alpha() == 255
        ? QStringLiteral("1")
        : QString::number(c.alphaF(), 'g', 3);
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a);
}

/// 色相、饱和度等一律用 F 版本再乘百分比：整数版本（0..255）换算回去会丢精度。
/// 灰阶色 Qt 可能给出 -1 色相，这里夹成 0，避免输出 hsl(-1, ...)。
inline QString formatHsl(const QColor &c)
{
    const int h = qRound(qMax<qreal>(0, c.hslHueF() * 360));
    return QStringLiteral("hsl(%1, %2%, %3%)")
        .arg(h).arg(qRound(c.hslSaturationF() * 100)).arg(qRound(c.lightnessF() * 100));
}

inline QString formatHsv(const QColor &c)
{
    const int h = qRound(qMax<qreal>(0, c.hsvHueF() * 360));
    return QStringLiteral("hsv(%1, %2%, %3%)")
        .arg(h).arg(qRound(c.hsvSaturationF() * 100)).arg(qRound(c.valueF() * 100));
}

/// CMYK 是屏幕 RGB 的近似换算（Qt 的 cyanF 等），不是印刷分色，
/// 面板里也按这个口径提示用户，避免被当成印厂可直接用的数值。
inline QString formatCmyk(const QColor &c)
{
    return QStringLiteral("cmyk(%1%, %2%, %3%, %4%)")
        .arg(qRound(c.cyanF() * 100)).arg(qRound(c.magentaF() * 100))
        .arg(qRound(c.yellowF() * 100)).arg(qRound(c.blackF() * 100));
}

/// 面板里逐行展示并复制的全部格式。增删格式只改这一处。
inline QList<Field> formatFields(const QColor &c)
{
    return {
        {QStringLiteral("HEX"),  formatHex(c)},
        {QStringLiteral("HEX8"), formatHexAlpha(c)},
        {QStringLiteral("RGB"),  formatRgb(c)},
        {QStringLiteral("RGBA"), formatRgba(c)},
        {QStringLiteral("HSL"),  formatHsl(c)},
        {QStringLiteral("HSV"),  formatHsv(c)},
        {QStringLiteral("CMYK"), formatCmyk(c)},
    };
}

/// WCAG 相对亮度（0 = 黑，1 = 白），用于决定色块上的文字该用深还是浅。
inline qreal luminance(const QColor &c)
{
    const auto linearize = [](qreal v) -> qreal {
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linearize(c.redF())
         + 0.7152 * linearize(c.greenF())
         + 0.0722 * linearize(c.blueF());
}

/// 把半透明颜色合成到白底上，得到「用户实际看到」的颜色。
/// 直接用带 alpha 的 RGB 算亮度会低估透明度，导致黑字落在深色背景上。
inline QColor overWhite(const QColor &c)
{
    const qreal a = c.alphaF();
    return QColor::fromRgbF(c.redF() * a + (1 - a),
                            c.greenF() * a + (1 - a),
                            c.blueF() * a + (1 - a));
}

inline qreal contrastRatio(const QColor &a, const QColor &b)
{
    const qreal l1 = luminance(overWhite(a));
    const qreal l2 = luminance(overWhite(b));
    const qreal hi = qMax(l1, l2);
    const qreal lo = qMin(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

/// 在给定底色上建议用黑字还是白字：取对比度更高的一侧。
/// 面板把结果和对比度一起显示，方便做无障碍配色检查。
inline QColor suggestedText(const QColor &background)
{
    static const QColor white(QStringLiteral("#FFFFFF"));
    static const QColor ink(QStringLiteral("#0F172A"));
    return contrastRatio(background, white) >= contrastRatio(background, ink) ? white : ink;
}

} // namespace colortext

#pragma once

// ---------------------------------------------------------------------------
// 主题色板：宿主与插件共用的「颜色令牌（token）」表 + 内联样式解析。
//
// 为什么放在 api/ 而不是 core/：插件只链接 xtools_api 这个纯头文件接口库，
// 看不到 core/；而插件面板里有不少内联样式与自绘颜色（JSON 语法色、状态行、
// 拖拽区…），必须和宿主用同一套令牌，否则深色模式下插件会留一块白底。
//
// 为什么状态不放在本文件的静态变量里：Windows 下每个 DLL 各有一份静态存储，
// 插件读到的是自己那份、永远停在初始值。当前主题一律通过 QApplication 的动态
// 属性传递（宿主 Theme 每次换肤时写入），这样跨模块读到的必然一致。
//
// 令牌名与 style.qss 里的 @token 一一对应：改色只改下面 table() 一处。
// ---------------------------------------------------------------------------

#include <QApplication>
#include <QColor>
#include <QHash>
#include <QRegularExpression>
#include <QString>
#include <QVector>
#include <QWidget>

#include <QVariant>

namespace xtools::theme {

/// 宿主写在 qApp 上的动态属性：true 表示当前是深色主题。
inline constexpr auto kDarkProperty = "xtoolsThemeDark";

/// 控件上的动态属性：保存「未解析的内联样式模板」。
/// 换肤时宿主据此重算颜色，否则插件里 setStyleSheet 过的控件会停在旧配色上。
inline constexpr auto kStyleProperty = "xtoolsThemeStyle";

/// 语义化颜色令牌。命名遵循「用途 + 深浅」而不是具体色相，
/// 这样换配色时不用改调用点（叫 "slate400" 的那种命名迟早会骗人）。
enum class Color
{
    Bg,                 ///< 主表面：窗口、卡片、输入框底色
    BgAlt,              ///< 下沉表面：只读字段、表格头、禁用态
    Card,               ///< 卡片表面：深色主题下比页面底稍亮，用来把卡片「抬」出来
    BgHover,            ///< 悬停底色
    BgPressed,          ///< 按下底色
    BgSubtle,           ///< 弱底色：胶囊、滚动条槽、行悬停
    BgTrack,            ///< 进度条槽
    AccentTint,         ///< 极浅的强调底色：建议条、拖拽区
    Border,             ///< 常规边框
    BorderSubtle,       ///< 更弱的分隔线
    BorderStrong,       ///< 输入框/按钮的实体边框
    BorderHover,        ///< 悬停时的边框
    BorderAccent,       ///< 强调色边框（浅）
    BorderAccentStrong, ///< 强调色边框（深）
    BorderAccentSoft,   ///< 虚线拖拽框
    BorderDanger,       ///< 危险操作按钮边框
    Text,               ///< 主文字
    TextBody,           ///< 正文
    TextSecond,         ///< 次要文字
    TextMuted,          ///< 说明文字
    TextFaint,          ///< 提示文字（最弱）
    TextDisabled,       ///< 禁用文字
    Accent,             ///< 强调色（主按钮、选中、焦点）
    AccentHover,        ///< 强调色悬停
    AccentDisabled,     ///< 强调色禁用
    AccentSoft,         ///< 选中行底色
    AccentSoftStrong,   ///< 选中行底色（更实一点）
    AccentText,         ///< 强调色文字
    Danger,             ///< 错误
    DangerStrong,       ///< 错误（实色块、关闭按钮）
    DangerText,         ///< 错误文字（深）
    DangerBg,           ///< 错误底色
    Success,            ///< 成功
    SuccessText,        ///< 成功文字（深）
    SuccessBg,          ///< 成功底色
    SuccessBorder,      ///< 成功胶囊边框
    Warn,               ///< 警告/占用偏高
    WarnText,           ///< 警告文字（深）
    WarnStrong,         ///< 警告（连接中、危险但可用）
    Teal,               ///< Cron 可视化编辑器的选中色
    QuoteUp,            ///< 行情上涨（A 股惯例：涨红跌绿，与欧美相反）
    QuoteDown,          ///< 行情下跌
    QuoteFlat,          ///< 行情持平 / 无数据
    Scrollbar,          ///< 滚动条滑块
    ScrollbarHover,     ///< 滚动条滑块悬停
    TooltipBg,          ///< 气泡底色（与主表面相反）
    TooltipText,        ///< 气泡文字
    Overlay,            ///< 浮层底色（吸管放大镜，带透明度）
    OnAccent,           ///< 实色块上的文字（两套主题都是白）
    Shadow,             ///< 窗口投影色（主窗口无边框圆角下的投影，深色下更不透明）
    SyntaxKey,          ///< JSON 键名
    SyntaxString,       ///< JSON 字符串
    SyntaxNumber,       ///< JSON 数字
    SyntaxLiteral,      ///< true/false/null
    SyntaxPunct,        ///< 标点
    SyntaxError,        ///< 语法错误
    JsonErrorBg,        ///< 语法错误处的高亮底色
    GutterBg,           ///< 编辑器行号槽底色
    GutterText,         ///< 行号
    GutterCurrent,      ///< 当前行行号
    GutterHoverBg,      ///< 行号槽悬停/折叠区底色
    MatchBg,            ///< 正则匹配底色
    MatchCurrentBg,     ///< 当前匹配底色
    DiffRemovedBg,      ///< 差异行（删除）
    DiffRemovedMark,    ///< 行内删除的字
    DiffAddedBg,        ///< 差异行（新增）
    DiffAddedMark,      ///< 行内新增的字
    DiffRemovedPaneBg,  ///< 差异面板（删除侧）底色
    DiffRemovedPaneBorder,
    DiffRemovedPaneText,
    DiffAddedPaneBg,
    DiffAddedPaneBorder,
    DiffAddedPaneText,
    DropActiveBg,       ///< 拖拽悬停时的投放区底色
    DropActiveBorder,   ///< 拖拽悬停时的投放区边框
};

/// 表项：令牌 id + 令牌名（与 qss 的 @name 对应）+ 浅色/深色取值。
struct Entry
{
    Color id;
    const char *name;
    const char *light;
    const char *dark;
};

/// 全量令牌表。新增颜色只改这里，然后就能在 qss 里写 @name、在代码里写
/// Color::Xxx。深色值刻意抬高了文字亮度、压低了底色饱和度：纯 #000 底 +
/// 纯白字在全屏时对比过强，长时间读会累。
inline const QVector<Entry> &table()
{
    static const QVector<Entry> entries = {
        {Color::Bg, "bg", "#FFFFFF", "#1B1E24"},
        {Color::BgAlt, "bg-alt", "#F8FAFC", "#14161B"},
        // 卡片表面：浅色与页面同为白（靠边框区分）；深色比页面 #1B1E24 略亮，
        // 让卡片从底色里浮出来。注意不要与 bg-subtle（#23272F）撞得太近，
        // 否则「卡片抬升」和「弱底色胶囊」两种语义在深色下分不开。
        {Color::Card, "card", "#FFFFFF", "#23272F"},
        {Color::BgHover, "bg-hover", "#F5F8FD", "#23272F"},
        {Color::BgPressed, "bg-pressed", "#EBF1F9", "#2A3038"},
        {Color::BgSubtle, "bg-subtle", "#F1F5F9", "#23272F"},
        {Color::BgTrack, "bg-track", "#EEF2F6", "#2A2E36"},
        {Color::AccentTint, "accent-tint", "#F6F8FF", "#1F2434"},
        {Color::Border, "border", "#E2E8F0", "#2E333D"},
        {Color::BorderSubtle, "border-subtle", "#E8EDF5", "#262A32"},
        {Color::BorderStrong, "border-strong", "#D7DEE9", "#3B414D"},
        {Color::BorderHover, "border-hover", "#C3CEDD", "#4B5361"},
        {Color::BorderAccent, "border-accent", "#C7D2FE", "#3E4A73"},
        {Color::BorderAccentStrong, "border-accent-strong", "#A5B4FC", "#55639B"},
        {Color::BorderAccentSoft, "border-accent-soft", "#A9C0FF", "#44517F"},
        {Color::BorderDanger, "border-danger", "#FECACA", "#5A2E2E"},
        {Color::Text, "text", "#0F172A", "#E6E9EF"},
        {Color::TextBody, "text-body", "#334155", "#C7CEDA"},
        {Color::TextSecond, "text-second", "#475569", "#A6AFBD"},
        {Color::TextMuted, "text-muted", "#64748B", "#8A94A3"},
        {Color::TextFaint, "text-faint", "#74839A", "#7A8494"},
        {Color::TextDisabled, "text-disabled", "#A8B3C2", "#5B6472"},
        {Color::Accent, "accent", "#4F6BF6", "#6C82F8"},
        {Color::AccentHover, "accent-hover", "#4360EE", "#7F92FA"},
        {Color::AccentDisabled, "accent-disabled", "#A9B8F8", "#4A5790"},
        {Color::AccentSoft, "accent-soft", "#EEF2FF", "#232B45"},
        {Color::AccentSoftStrong, "accent-soft-strong", "#E6EBFF", "#2B3554"},
        {Color::AccentText, "accent-text", "#1D4ED8", "#A9B8FF"},
        {Color::Danger, "danger", "#DC2626", "#F87171"},
        {Color::DangerStrong, "danger-strong", "#EF4444", "#F87171"},
        {Color::DangerText, "danger-text", "#B91C1C", "#FCA5A5"},
        {Color::DangerBg, "danger-bg", "#FEF2F2", "#2A1D1E"},
        {Color::Success, "success", "#10B981", "#34D399"},
        {Color::SuccessText, "success-text", "#047857", "#6EE7B7"},
        {Color::SuccessBg, "success-bg", "#ECFDF5", "#14241F"},
        {Color::SuccessBorder, "success-border", "#A7F3D0", "#1F4438"},
        {Color::Warn, "warn", "#F59E0B", "#FBBF24"},
        {Color::WarnText, "warn-text", "#B45309", "#FBBF24"},
        {Color::WarnStrong, "warn-strong", "#D97706", "#FBBF24"},
        {Color::Teal, "teal", "#0F766E", "#0F766E"},
        // 行情涨跌色单独挑，不复用 danger/success：那两套是「错误/成功」的语义色，
        // 深色主题下 success 偏青绿、danger 偏橙红，放在满屏数字里会显得发灰。
        {Color::QuoteUp, "quote-up", "#D92B2B", "#FF7A7A"},
        {Color::QuoteDown, "quote-down", "#12915C", "#4ADE80"},
        {Color::QuoteFlat, "quote-flat", "#64748B", "#8A94A3"},
        {Color::Scrollbar, "scrollbar", "#CBD5E1", "#3C434F"},
        {Color::ScrollbarHover, "scrollbar-hover", "#A9B7C9", "#55606F"},
        {Color::TooltipBg, "tooltip-bg", "#0F172A", "#2C313A"},
        {Color::TooltipText, "tooltip-text", "#F8FAFC", "#E6E9EF"},
        {Color::Overlay, "overlay", "rgba(255,255,255,235)", "rgba(26,29,35,238)"},
        {Color::OnAccent, "on-accent", "#FFFFFF", "#FFFFFF"},
        // 投影色：浅色主题用偏深的 slate 阴影，深色主题用纯黑（黑底上阴影更深才看得出）。
        // 实际透明度在 C++ 里用 setAlpha 调（浅 ~90 / 深 ~180），这里只给基色。
        {Color::Shadow, "shadow", "#0F172A", "#000000"},
        {Color::SyntaxKey, "syntax-key", "#1D4ED8", "#93B4FF"},
        {Color::SyntaxString, "syntax-string", "#047857", "#7EE2B8"},
        {Color::SyntaxNumber, "syntax-number", "#B45309", "#FBBF24"},
        {Color::SyntaxLiteral, "syntax-literal", "#7C3AED", "#C9A7FF"},
        {Color::SyntaxPunct, "syntax-punct", "#94A3B8", "#7A8494"},
        {Color::SyntaxError, "syntax-error", "#DC2626", "#F87171"},
        {Color::JsonErrorBg, "json-error-bg", "#FEE2E2", "#45262A"},
        {Color::GutterBg, "gutter-bg", "#FBFCFD", "#171A20"},
        {Color::GutterText, "gutter-text", "#A3AEBD", "#5F6A78"},
        {Color::GutterCurrent, "gutter-current", "#334155", "#C7CEDA"},
        {Color::GutterHoverBg, "gutter-hover-bg", "#E8EDFB", "#2B3554"},
        {Color::MatchBg, "match-bg", "#FDE68A", "#6B4A00"},
        {Color::MatchCurrentBg, "match-current-bg", "#FBBF24", "#A16207"},
        {Color::DiffRemovedBg, "diff-removed-bg", "#FEE2E2", "#3A1D1D"},
        {Color::DiffRemovedMark, "diff-removed-mark", "#FCA5A5", "#7F2B2B"},
        {Color::DiffAddedBg, "diff-added-bg", "#DCFCE7", "#14351F"},
        {Color::DiffAddedMark, "diff-added-mark", "#86EFAC", "#245C38"},
        {Color::DiffRemovedPaneBg, "diff-removed-pane-bg", "#FEF6F6", "#241A1A"},
        {Color::DiffRemovedPaneBorder, "diff-removed-pane-border", "#F1DADA", "#4A2A2A"},
        {Color::DiffRemovedPaneText, "diff-removed-pane-text", "#7F1D1D", "#FCA5A5"},
        {Color::DiffAddedPaneBg, "diff-added-pane-bg", "#F4FDF7", "#14221A"},
        {Color::DiffAddedPaneBorder, "diff-added-pane-border", "#D6EFDF", "#24463A"},
        {Color::DiffAddedPaneText, "diff-added-pane-text", "#14532D", "#86EFAC"},
        {Color::DropActiveBg, "drop-active-bg", "#EFF6FF", "#1F2B4A"},
        {Color::DropActiveBorder, "drop-active-border", "#2563EB", "#5B7BF5"},
    };
    return entries;
}

/// 当前是否深色主题。属性缺失（未初始化、或独立小程序）时按浅色处理。
inline bool isDark()
{
    return qApp && qApp->property(kDarkProperty).toBool();
}

/// 令牌名 → 颜色值。返回的 QHash 按深/浅各建一次，之后只读复用。
inline const QHash<QString, QString> &palette(bool dark)
{
    static const QHash<QString, QString> lightPalette = [] {
        QHash<QString, QString> map;
        for (const Entry &e : table())
            map.insert(QString::fromLatin1(e.name), QString::fromLatin1(e.light));
        return map;
    }();
    static const QHash<QString, QString> darkPalette = [] {
        QHash<QString, QString> map;
        for (const Entry &e : table())
            map.insert(QString::fromLatin1(e.name), QString::fromLatin1(e.dark));
        return map;
    }();
    return dark ? darkPalette : lightPalette;
}

/// 令牌 id → 令牌名。表很小，线性查找足够，还能省掉一张反向表。
inline QString tokenName(Color token)
{
    for (const Entry &e : table()) {
        if (e.id == token)
            return QString::fromLatin1(e.name);
    }
    return QString();
}

/// 令牌 id → 当前主题下的颜色值（"#RRGGBB" 或 "rgba(...)"）。
inline QString hex(Color token)
{
    return palette(isDark()).value(tokenName(token));
}

/// 令牌 id → QColor。自绘（QPainter）场景用这个。
inline QColor color(Color token)
{
    return QColor(hex(token));
}

/// 把 "颜色:@text-faint;font-size:12px;" 这类片段里的 @token 换成当前主题的色值。
/// 未知令牌原样保留并返回，方便在调试时直接看出哪里写错了（Qt 会忽略该条规则，
/// 表现为控件沿用默认样式，而不是整个样式表崩掉）。
inline QString resolve(const QString &pattern)
{
    if (!pattern.contains(QLatin1Char('@')))
        return pattern;

    static const QRegularExpression tokenRe(QStringLiteral("@([a-z0-9-]+)"));
    const QHash<QString, QString> &map = palette(isDark());

    QString out;
    out.reserve(pattern.size());
    int last = 0;
    auto it = tokenRe.globalMatch(pattern);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString value = map.value(match.captured(1));
        if (value.isEmpty())
            continue;   // 未知令牌：保持原样，让上面的约定生效
        out += pattern.mid(last, match.capturedStart() - last);
        out += value;
        last = match.capturedEnd();
    }
    out += pattern.mid(last);
    return out;
}

/// 设置控件样式：模板存进动态属性，解析后的结果交给 setStyleSheet。
///
/// 关键点在"存模板"：插件面板是独立编译的动态库，宿主换肤时只能看到它的控件，
/// 看不到当初拼的字符串。把模板挂在控件上，宿主就能遍历所有控件重算颜色，
/// 于是「切到深色」不需要重启、也不会在插件里留一块白底。
inline void applyStyle(QWidget *widget, const QString &pattern)
{
    if (!widget)
        return;
    widget->setProperty(kStyleProperty, pattern);
    widget->setStyleSheet(resolve(pattern));
}

} // namespace xtools::theme

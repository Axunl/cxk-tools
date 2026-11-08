#pragma once

// ---------------------------------------------------------------------------
// cron 表达式纯逻辑：解析、中文描述、下次运行时间、由字段文本拼装、
// 可视化编辑状态（FieldEdit）与字段原文的双向互转。不依赖任何 Qt Widgets，
// 可脱离界面用临时工程直接断言（AGENTS.md 规则 2）。
//
// 支持两种段数（按段数自动识别）：
//   5 段（标准 crontab）：分 时 日 月 周
//   6 段（Spring / Quartz）：秒 分 时 日 月 周
//
// 字段语法（逗号列表可混合任意项）：
//   *        任意值（Quartz 的 ? 在日/周字段等价于 *，其他字段报错）
//   a        单值
//   a-b      闭区间
//   a-b/n    区间内步长
//   a/n      从 a 到字段最大值、步长 n（Vixie 语义）
//   */n      全范围步长 n
//   名称     月 JAN..DEC、周 SUN..SAT，大小写不敏感；周另接受 0~7（0 与 7 都是周日）
//
// 不支持 L / W / # 等 Quartz 扩展：遇到直接报错，不猜语义。
//
// 日/周并用语义（Vixie cron 手册，Linux crontab 的真实行为）：
//   日、周两个字段都是「受限」（原文不是 * 也不是 ?）时，任一满足即执行；
//   否则两者取交集。因此 Quartz 风格「一边写 ?」的表达式也落在交集分支，行为正确。
//
// 错误处理：失败写 error 输出参数并返回 false（项目不用异常）。
// ---------------------------------------------------------------------------

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

#include <set>

namespace cron {

/// 字段序号。5 段输入时 Second 内部补 "0"（标准 cron 在分钟的第 0 秒触发）。
enum class FieldType { Second, Minute, Hour, DayOfMonth, Month, DayOfWeek };

/// 字段里逗号分隔的一个片段（已把 a/n 归一成 a-max/n）。
struct Segment
{
    bool wildcard = false;   ///< 整片是 * 或 ?（"*/n" 也算 wildcard，只是 step > 1）
    bool openEnded = false;  ///< 用户写的是 "a/n"（起点固定直到最大值），描述措辞不同
    bool isRange = false;    ///< true 表示 lo~hi 区间；false 表示单值（lo == hi）
    int lo = 0;
    int hi = 0;
    int step = 1;
};

/// 单个字段的解析结果。
struct FieldSpec
{
    QString raw;             ///< 归一后的原文（去空白、转小写），回填输入框用它
    bool wildcard = false;   ///< 原文是 * 或 ?。日/周并用语义靠它判断「是否受限」
    bool valid = true;
    QString error;
    std::set<int> values;    ///< 展开后的取值集合（周日归一为 0）
    QVector<Segment> segments;
};

/// 完整表达式的解析结果。字段顺序固定为：秒 分 时 日 月 周。
struct Spec
{
    bool hasSeconds = false; ///< 用户输入是 6 段（Spring / Quartz 风格）
    bool valid = true;
    QString error;
    FieldSpec second, minute, hour, dom, mon, dow;
};

// ------------------------------------------------------------ 字段元信息

/// 字段中文名（错误信息与输入框标签共用）。
inline QString fieldTypeName(FieldType type)
{
    switch (type) {
    case FieldType::Second:     return QStringLiteral("秒");
    case FieldType::Minute:     return QStringLiteral("分");
    case FieldType::Hour:       return QStringLiteral("时");
    case FieldType::DayOfMonth: return QStringLiteral("日");
    case FieldType::Month:      return QStringLiteral("月");
    case FieldType::DayOfWeek:  return QStringLiteral("周");
    }
    return {};
}

/// 字段取值范围，用于越界校验与输入框提示。周是 0~7（7 == 0 == 周日）。
inline void fieldRange(FieldType type, int *min, int *max)
{
    switch (type) {
    case FieldType::Second:
    case FieldType::Minute:     *min = 0;  *max = 59; break;
    case FieldType::Hour:       *min = 0;  *max = 23; break;
    case FieldType::DayOfMonth: *min = 1;  *max = 31; break;
    case FieldType::Month:      *min = 1;  *max = 12; break;
    case FieldType::DayOfWeek:  *min = 0;  *max = 7;  break;
    }
}

/// 输入框悬浮提示（范围 + 语法 + 示例），面板直接用。
inline QString fieldHint(FieldType type)
{
    switch (type) {
    case FieldType::Second:
        return QStringLiteral("0~59。* / a / a-b / a-b/n / a/n，如 0、*/10、30-50/5");
    case FieldType::Minute:
        return QStringLiteral("0~59。* / a / a-b / a-b/n / a/n，如 0、*/5、0-30/10");
    case FieldType::Hour:
        return QStringLiteral("0~23。* / a / a-b / a/n，如 9、9-18、*/2");
    case FieldType::DayOfMonth:
        return QStringLiteral("1~31。* / ? / a / a-b / a/n，如 1、1,15、1-10、*/2");
    case FieldType::Month:
        return QStringLiteral("1~12，可用名称 JAN~DEC。如 1、1,7、1-6、*/3");
    case FieldType::DayOfWeek:
        return QStringLiteral("0~7（0 与 7 都是周日），可用名称 SUN~SAT。如 1、1-5、MON-FRI");
    }
    return {};
}

/// 周值转中文名。v 是 0~6（7 与 0 都是周日，展示前归一）。
inline QString dowName(int v)
{
    if (v == 7)
        v = 0;
    static const char *kNames[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (v < 0 || v > 6)
        return QString::number(v);
    return QString::fromUtf8(kNames[v]);
}

// ------------------------------------------------------------ 解析

namespace detail {

/// 解析单个数值 token：纯数字，或月/周字段的英文名。失败写 error。
inline bool parseValueToken(QString token, FieldType type, int *out, QString *error)
{
    token = token.trimmed();
    if (token.isEmpty()) {
        if (error)
            *error = QStringLiteral("缺少数值");
        return false;
    }

    // 纯数字（不接受负号与小数）
    bool isDigits = true;
    for (const QChar c : token) {
        if (!c.isDigit()) {
            isDigits = false;
            break;
        }
    }
    if (isDigits) {
        bool ok = false;
        *out = token.toInt(&ok);
        if (!ok) {
            if (error)
                *error = QStringLiteral("数值过大：%1").arg(token);
            return false;
        }
        return true;
    }

    // 名称：只有月（JAN..DEC）与周（SUN..SAT）有英文名
    const QString name = token.toUpper();
    if (type == FieldType::Month) {
        static const char *kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        for (int i = 0; i < 12; ++i) {
            if (name == QLatin1String(kMonths[i])) {
                *out = i + 1;
                return true;
            }
        }
        if (error)
            *error = QStringLiteral("未知月份名：%1（可用 JAN~DEC）").arg(token);
        return false;
    }
    if (type == FieldType::DayOfWeek) {
        static const char *kDows[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        for (int i = 0; i < 7; ++i) {
            if (name == QLatin1String(kDows[i])) {
                *out = i;
                return true;
            }
        }
        if (error)
            *error = QStringLiteral("未知星期名：%1（可用 SUN~SAT）").arg(token);
        return false;
    }

    if (error)
        *error = QStringLiteral("无法识别的数值：%1").arg(token);
    return false;
}

/// 把一个片段的取值展开进 out（步长、周 7→0 归一都在这里处理）。
inline void expandSegment(const Segment &seg, FieldType type, std::set<int> *out)
{
    int vmin = 0, vmax = 0;
    fieldRange(type, &vmin, &vmax);
    const int lo = seg.wildcard ? vmin : seg.lo;
    const int hi = seg.wildcard ? vmax : seg.hi;
    for (int v = lo; v <= hi; v += seg.step) {
        int nv = v;
        if (type == FieldType::DayOfWeek && nv == 7)
            nv = 0; // 7 与 0 都是周日，统一存 0，匹配时只查一次
        out->insert(nv);
    }
}

} // namespace detail

/// 解析单个字段。任何失败都返回 valid=false 并写好中文原因。
inline FieldSpec parseField(const QString &text, FieldType type)
{
    FieldSpec f;
    f.raw = text.trimmed().toLower();
    if (f.raw.isEmpty()) {
        f.valid = false;
        f.error = QStringLiteral("字段为空");
        return f;
    }

    int vmin = 0, vmax = 0;
    fieldRange(type, &vmin, &vmax);

    // 通配判定只认整段 "*" / "?"：按 Vixie 手册，"*/n" 仍算受限字段，
    // 日/周并用语义（见文件头）依赖这个区分。
    f.wildcard = (f.raw == QLatin1String("*") || f.raw == QLatin1String("?"));

    const QStringList parts = f.raw.split(QLatin1Char(','));
    for (const QString &rawPart : parts) {
        if (rawPart.trimmed().isEmpty()) {
            f.valid = false;
            f.error = QStringLiteral("存在空的列表项");
            return f;
        }

        Segment seg;
        QString body = rawPart.trimmed();

        // 步长：a-b/n、a/n、*/n。只允许一个 '/'（第二个留给报错）
        const int slash = body.indexOf(QLatin1Char('/'));
        if (slash >= 0) {
            const QString stepText = body.mid(slash + 1);
            body = body.left(slash);
            if (stepText.contains(QLatin1Char('/'))) {
                f.valid = false;
                f.error = QStringLiteral("步长格式错误：%1").arg(rawPart.trimmed());
                return f;
            }
            bool ok = false;
            seg.step = stepText.toInt(&ok);
            if (!ok) {
                f.valid = false;
                f.error = QStringLiteral("步长不是整数：%1").arg(stepText);
                return f;
            }
            if (seg.step < 1) {
                f.valid = false;
                f.error = QStringLiteral("步长必须大于 0：%1").arg(stepText);
                return f;
            }
        }

        body = body.trimmed();
        if (body == QLatin1String("*") || body == QLatin1String("?")) {
            if (body == QLatin1String("?") && type != FieldType::DayOfMonth
                && type != FieldType::DayOfWeek) {
                f.valid = false;
                f.error = QStringLiteral("'?' 只能用在日或周字段");
                return f;
            }
            seg.wildcard = true;
        } else if (body.contains(QLatin1Char('-'))) {
            const int dash = body.indexOf(QLatin1Char('-'));
            int lo = 0, hi = 0;
            QString err;
            if (!detail::parseValueToken(body.left(dash), type, &lo, &err)
                || !detail::parseValueToken(body.mid(dash + 1), type, &hi, &err)) {
                f.valid = false;
                f.error = err;
                return f;
            }
            // 周字段 7 与 0 都是周日：这里保留原值参与区间（"5-7" = 周五到周日），
            // 折叠到 0 统一放在 expandSegment，否则 "5-7" 会被拆成倒序区间报错
            if (lo > hi) {
                f.valid = false;
                f.error = QStringLiteral("区间起点大于终点：%1").arg(rawPart.trimmed());
                return f;
            }
            if (lo < vmin || hi > vmax) {
                f.valid = false;
                f.error = QStringLiteral("超出范围（%1~%2）：%3").arg(vmin).arg(vmax).arg(rawPart.trimmed());
                return f;
            }
            seg.isRange = true;
            seg.lo = lo;
            seg.hi = hi;
        } else {
            int v = 0;
            QString err;
            if (!detail::parseValueToken(body, type, &v, &err)) {
                f.valid = false;
                f.error = err;
                return f;
            }
            if (v < vmin || v > vmax) {
                f.valid = false;
                f.error = QStringLiteral("超出范围（%1~%2）：%3").arg(vmin).arg(vmax).arg(rawPart.trimmed());
                return f;
            }
            // 单值也要写 hi：expand 按 [lo, hi] 闭区间展开，hi 停留在默认 0 会
            // 让 "日=1" 这类单值展开成空集合（下次运行永远匹配不上）
            seg.lo = v;
            seg.hi = v;
            if (seg.step > 1) {
                // "a/n" 等价 a-max/n：起点固定、直到字段最大值
                seg.openEnded = true;
                seg.isRange = true;
                seg.hi = vmax;
            }
        }

        f.segments.append(seg);
        detail::expandSegment(seg, type, &f.values);
    }

    return f;
}

/// 解析完整表达式（5 或 6 段，空白分隔）。失败时 valid=false 且 error 里写明是哪个字段。
inline Spec parse(const QString &expr)
{
    Spec s;
    const QString trimmed = expr.simplified();
    if (trimmed.isEmpty()) {
        s.valid = false;
        s.error = QStringLiteral("表达式为空");
        return s;
    }

    const QStringList parts = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 5 && parts.size() != 6) {
        s.valid = false;
        s.error = QStringLiteral("需要 5 段（分 时 日 月 周）或 6 段（秒 分 时 日 月 周），当前 %1 段")
                      .arg(parts.size());
        return s;
    }

    s.hasSeconds = (parts.size() == 6);
    int idx = 0;
    auto take = [&](FieldType type) -> FieldSpec {
        return parseField(parts.at(idx++), type);
    };

    if (s.hasSeconds)
        s.second = take(FieldType::Second);
    else
        s.second = parseField(QStringLiteral("0"), FieldType::Second); // 标准 cron 在第 0 秒触发

    s.minute = take(FieldType::Minute);
    s.hour = take(FieldType::Hour);
    s.dom = take(FieldType::DayOfMonth);
    s.mon = take(FieldType::Month);
    s.dow = take(FieldType::DayOfWeek);

    const struct { const FieldSpec *f; FieldType t; } kAll[] = {
        {&s.second, FieldType::Second},   {&s.minute, FieldType::Minute},
        {&s.hour, FieldType::Hour},       {&s.dom, FieldType::DayOfMonth},
        {&s.mon, FieldType::Month},       {&s.dow, FieldType::DayOfWeek},
    };
    for (const auto &item : kAll) {
        // 5 段输入时秒是内部补的，报错也不该轮到它
        if (!s.hasSeconds && item.t == FieldType::Second)
            continue;
        if (!item.f->valid) {
            s.valid = false;
            s.error = QStringLiteral("%1 字段：%2").arg(fieldTypeName(item.t), item.f->error);
            return s;
        }
    }
    return s;
}

/// 用各字段原文拼装表达式（先逐字段校验，全合法才返回拼接结果）。
/// @param fields 5 个或 6 个字段文本，顺序与 parse 相同
/// @param error 失败原因（可为 nullptr）
/// @return 合法表达式；失败返回空串
inline QString build(const QStringList &fields, QString *error = nullptr)
{
    if (fields.size() != 5 && fields.size() != 6) {
        if (error)
            *error = QStringLiteral("需要 5 个或 6 个字段，当前 %1 个").arg(fields.size());
        return {};
    }
    QStringList cleaned;
    for (int i = 0; i < fields.size(); ++i) {
        const QString text = fields.at(i).trimmed();
        // 6 段的第 0 段是秒，5 段的第 0 段是分……按位置映射即可
        FieldType type;
        if (fields.size() == 6)
            type = static_cast<FieldType>(i);
        else
            type = static_cast<FieldType>(i + 1); // 跳过 Second

        const FieldSpec f = parseField(text, type);
        if (!f.valid) {
            if (error)
                *error = QStringLiteral("%1 字段：%2").arg(fieldTypeName(type), f.error);
            return {};
        }
        cleaned << f.raw;
    }
    return cleaned.join(QLatin1Char(' '));
}

// ------------------------------------------------------------ 描述

namespace detail {

/// 单值文本：数值字段给数字，周给中文名。
inline QString valueText(FieldType type, int v)
{
    if (type == FieldType::DayOfWeek)
        return dowName(v);
    return QString::number(v);
}

/// 单值/区间后的单位后缀（前面带空格）。周的名称自带含义，不加后缀。
inline QString unitSuffix(FieldType type)
{
    switch (type) {
    case FieldType::Second:     return QStringLiteral(" 秒");
    case FieldType::Minute:     return QStringLiteral(" 分");
    case FieldType::Hour:       return QStringLiteral(" 点");
    case FieldType::DayOfMonth: return QStringLiteral(" 号");
    case FieldType::Month:      return QStringLiteral(" 月");
    case FieldType::DayOfWeek:  return {};
    }
    return {};
}

/// 「每 N …」里的量词（分钟/小时/天/个月）。
inline QString unitWord(FieldType type)
{
    switch (type) {
    case FieldType::Second:     return QStringLiteral("秒");
    case FieldType::Minute:     return QStringLiteral("分钟");
    case FieldType::Hour:       return QStringLiteral("小时");
    case FieldType::DayOfMonth: return QStringLiteral("天");
    case FieldType::Month:      return QStringLiteral("个月");
    case FieldType::DayOfWeek:  return QStringLiteral("天");
    }
    return {};
}

/// 字段整体通配时的说法。
inline QString everyWord(FieldType type)
{
    switch (type) {
    case FieldType::Second:     return QStringLiteral("每秒");
    case FieldType::Minute:     return QStringLiteral("每分钟");
    case FieldType::Hour:       return QStringLiteral("每小时");
    case FieldType::DayOfMonth: return QStringLiteral("每天");
    case FieldType::Month:      return QStringLiteral("每月");
    case FieldType::DayOfWeek:  return QStringLiteral("每周任意天");
    }
    return {};
}

/// 单个片段的中文说法。
inline QString segmentPhrase(FieldType type, const Segment &seg)
{
    if (seg.wildcard) {
        if (seg.step <= 1)
            return everyWord(type); // 列表里的裸 *（如 "*,5"），极少见
        return QStringLiteral("每 %1 %2").arg(seg.step).arg(unitWord(type));
    }
    if (!seg.isRange && seg.step <= 1)
        return valueText(type, seg.lo) + unitSuffix(type);
    if (seg.openEnded)
        return QStringLiteral("从 %1 起每 %2 %3")
            .arg(valueText(type, seg.lo) + unitSuffix(type))
            .arg(seg.step)
            .arg(unitWord(type));
    const QString span = valueText(type, seg.lo) + QLatin1Char('~') + valueText(type, seg.hi);
    if (seg.step <= 1)
        return span + unitSuffix(type);
    return QStringLiteral("%1 内每 %2 %3").arg(span + unitSuffix(type)).arg(seg.step).arg(unitWord(type));
}

} // namespace detail

/// 字段的中文说法，例如 "每 5 分钟"、"0、30 分"、"0~30 分"、"9 点"、"周一"。
inline QString fieldPhrase(FieldType type, const FieldSpec &f)
{
    if (f.wildcard && f.segments.size() == 1 && f.segments.first().step <= 1)
        return detail::everyWord(type);

    // 全是单值 → 数字合并、单位只写一次："0、30 分"（比 "0 分、30 分" 干净）
    bool allSimple = true;
    for (const Segment &seg : f.segments) {
        if (seg.wildcard || seg.isRange || seg.step > 1) {
            allSimple = false;
            break;
        }
    }
    if (allSimple) {
        QStringList vs;
        for (const Segment &seg : f.segments)
            vs << detail::valueText(type, seg.lo);
        return vs.join(QStringLiteral("、")) + detail::unitSuffix(type);
    }

    QStringList parts;
    for (const Segment &seg : f.segments)
        parts << detail::segmentPhrase(type, seg);
    return parts.join(QStringLiteral("、"));
}

namespace detail {

/// 周字段的整段说法：常见组合给意译，其余按值列举。
inline QString dowPhrase(const FieldSpec &f)
{
    if (f.values == std::set<int>({1, 2, 3, 4, 5}))
        return QStringLiteral("工作日");
    if (f.values == std::set<int>({0, 6}))
        return QStringLiteral("周末");
    // 带步长的周字段（如 */2）按值展开更直观："每周日、周二、周四、周六"
    bool hasStep = false;
    for (const Segment &seg : f.segments) {
        if (seg.step > 1)
            hasStep = true;
    }
    if (hasStep) {
        QStringList names;
        for (int v : f.values)
            names << dowName(v);
        return QStringLiteral("每") + names.join(QStringLiteral("、"));
    }
    return QStringLiteral("每") + fieldPhrase(FieldType::DayOfWeek, f);
}

/// 日期部分（月/日/周）。orSemantics 置位表示日与周都受限（任一满足即执行）。
inline QString datePhrase(const Spec &s, bool *orSemantics)
{
    const bool domW = s.dom.wildcard;
    const bool dowW = s.dow.wildcard;
    const bool monW = s.mon.wildcard;
    if (orSemantics)
        *orSemantics = (!domW && !dowW);

    if (domW && dowW && monW)
        return {};

    QString monPart;
    if (!monW)
        monPart = fieldPhrase(FieldType::Month, s.mon);

    if (domW && dowW) {
        // 只限了月份：日期语境由装配层按需补，这里直接给 "7 月每天"
        return monPart + QStringLiteral("每天");
    }
    if (!domW && dowW) {
        const QString domPart = fieldPhrase(FieldType::DayOfMonth, s.dom);
        return monW ? QStringLiteral("每月 ") + domPart
                    : monPart + QLatin1Char(' ') + domPart;
    }
    if (domW && !dowW) {
        const QString dowPart = dowPhrase(s.dow);
        return monW ? dowPart : monPart + QStringLiteral("的") + dowPart;
    }
    // 日、周都受限：Vixie 语义是「任一满足」，描述里必须点明
    const QString base = fieldPhrase(FieldType::DayOfMonth, s.dom) + QStringLiteral("或") + dowPhrase(s.dow);
    return monW ? QStringLiteral("每月 ") + base : monPart + QLatin1Char(' ') + base;
}

/// 时间部分（时/分/秒）。showSec 仅在用户写了 6 段且秒不是 "0" 时成立——
/// "0" 是 5 段 cron 的默认触发时刻，说出来只有噪音。
///
/// 措辞分支用「是不是 * 形片段」（含 */n）划分，而不是 parse 的 wildcard 标志：
/// "*/5" 在 Vixie 语义里是受限字段（日/周并用判断要区分），但描述上它就是
/// "每 5 分钟"，落进「每小时的 …」会变成「每小时的每 5 分钟」这种怪话。
inline QString timePhrase(const Spec &s)
{
    const bool showSec = s.hasSeconds && s.second.raw != QLatin1String("0");

    const auto isSingle = [](const FieldSpec &f) {
        return f.segments.size() == 1 && !f.segments.first().wildcard
               && !f.segments.first().isRange && f.segments.first().step <= 1;
    };
    const auto isStarSeg = [](const FieldSpec &f) {
        return f.segments.size() == 1 && f.segments.first().wildcard;
    };

    const bool hourStar = isStarSeg(s.hour); // * 或 */n
    const bool minStar = isStarSeg(s.minute);
    const int hourStep = hourStar ? s.hour.segments.first().step : 1;
    const int minStep = minStar ? s.minute.segments.first().step : 1;

    // 时、分都是具体取值
    if (!hourStar && !minStar) {
        // 全单值 → 直接钟表格式，扫一眼就知道触发点
        if (isSingle(s.hour) && isSingle(s.minute) && (!showSec || isSingle(s.second))) {
            QString clock = QStringLiteral("%1:%2")
                                .arg(s.hour.segments.first().lo, 2, 10, QChar('0'))
                                .arg(s.minute.segments.first().lo, 2, 10, QChar('0'));
            if (showSec)
                clock += QStringLiteral(":%1").arg(s.second.segments.first().lo, 2, 10, QChar('0'));
            return clock;
        }
        QString t = fieldPhrase(FieldType::Hour, s.hour) + QStringLiteral(" 的 ")
                    + fieldPhrase(FieldType::Minute, s.minute);
        if (showSec)
            t += QStringLiteral(" 的 ") + fieldPhrase(FieldType::Second, s.second);
        return t;
    }

    // 时具体、分是 * 形："9 点内每分钟" / "9 点内每 5 分钟"
    if (!hourStar && minStar) {
        QString t = fieldPhrase(FieldType::Hour, s.hour) + QStringLiteral("内")
                    + fieldPhrase(FieldType::Minute, s.minute);
        if (showSec)
            t += QStringLiteral("（") + fieldPhrase(FieldType::Second, s.second) + QStringLiteral("）");
        return t;
    }

    // 时、分都是 * 形：只剩节奏
    if (hourStar && minStar) {
        if (hourStep <= 1 && minStep <= 1) {
            if (!showSec)
                return QStringLiteral("每分钟");
            if (s.second.wildcard)
                return QStringLiteral("每秒");
            const QString secPart = fieldPhrase(FieldType::Second, s.second);
            if (secPart.startsWith(QStringLiteral("每")))
                return secPart; // "每 5 秒"
            return QStringLiteral("每分钟的 ") + secPart; // "每分钟的 15 秒"
        }
        if (hourStep <= 1 && minStep > 1)
            return fieldPhrase(FieldType::Minute, s.minute); // "每 5 分钟"
        return fieldPhrase(FieldType::Hour, s.hour) + QStringLiteral("内")
               + fieldPhrase(FieldType::Minute, s.minute); // "每 2 小时内每 5 分钟"
    }

    // 时是 * 形、分具体："每小时整点" / "每小时的 0、30 分"
    if (hourStep <= 1) {
        if (!showSec && isSingle(s.minute) && s.minute.segments.first().lo == 0)
            return QStringLiteral("每小时整点");
        QString t = QStringLiteral("每小时的 ") + fieldPhrase(FieldType::Minute, s.minute);
        if (showSec)
            t += QStringLiteral("（") + fieldPhrase(FieldType::Second, s.second) + QStringLiteral("）");
        return t;
    }
    return fieldPhrase(FieldType::Hour, s.hour) + QStringLiteral(" 的 ")
           + fieldPhrase(FieldType::Minute, s.minute); // "每 2 小时的 0 分"
}

} // namespace detail

/// 完整中文描述，例如 "每 5 分钟执行一次"、"工作日 09:00 执行"、"每月 1 号 08:30 执行"。
/// spec 无效时返回空串。
inline QString describe(const Spec &s)
{
    if (!s.valid)
        return {};

    bool orSemantics = false;
    QString date = detail::datePhrase(s, &orSemantics);
    const QString time = detail::timePhrase(s);

    QString core;
    if (date.isEmpty()) {
        core = time.startsWith(QStringLiteral("每")) ? time : QStringLiteral("每天 ") + time;
    } else if (time.startsWith(QStringLiteral("每"))) {
        // "7 月每天每 5 分钟" 里的「每天」是噪音，循环型时间自带频率语义
        if (date.endsWith(QStringLiteral("每天")))
            date.chop(2);
        core = date + time;
    } else {
        core = date + QLatin1Char(' ') + time;
    }

    // 纯循环型（"每 5 分钟"）收"执行一次"；其余收"执行"。
    // 钟表型（"09:00"）前面补一个空格，中文数字紧贴动词读起来黏成一团。
    const bool pureRecurrence = date.isEmpty() && time.startsWith(QStringLiteral("每"))
                                && (time.endsWith(QStringLiteral("分钟"))
                                    || time.endsWith(QStringLiteral("秒"))
                                    || time.endsWith(QStringLiteral("小时")));
    QString out;
    if (pureRecurrence) {
        out = core + QStringLiteral("执行一次");
    } else {
        const QChar last = core.isEmpty() ? QChar() : core.at(core.size() - 1);
        const bool clockLike = last.isDigit();
        out = core + (clockLike ? QStringLiteral(" 执行") : QStringLiteral("执行"));
    }

    if (orSemantics)
        out += QStringLiteral("（日与周任一满足即执行）");
    return out;
}

// ------------------------------------------------------------ 下次运行

namespace detail {

/// Qt 的 dayOfWeek（周一 1 ~ 周日 7）→ cron 的周日 0 ~ 周六 6。
inline int cronDow(const QDate &date)
{
    return date.dayOfWeek() % 7;
}

/// 日/周并用语义：都受限时任一满足即执行，否则交集（见文件头）。
inline bool dayMatches(const Spec &s, const QDate &date)
{
    const bool domW = s.dom.wildcard;
    const bool dowW = s.dow.wildcard;
    const bool domOk = domW || s.dom.values.count(date.day()) > 0;
    const bool dowOk = dowW || s.dow.values.count(cronDow(date)) > 0;
    if (!domW && !dowW)
        return domOk || dowOk;
    return domOk && dowOk;
}

} // namespace detail

/// 计算 from 之后的最多 count 次运行时刻（本地时区）。
///
/// 按「先跳月、再跳日、日内枚举时分秒」推进：不匹配的月份/日子整段跳过，
/// 不会退化成逐分钟扫描。返回不足 count 个（甚至为空）说明表达式在 5 年内
/// 不触发（如 "0 0 30 2 *"），调用方应给出明确提示而不是报错。
inline QVector<QDateTime> nextRuns(const Spec &spec, const QDateTime &from, int count)
{
    QVector<QDateTime> out;
    if (!spec.valid || count <= 0)
        return out;

    // 游标对齐到 from 之后的第一个候选时刻
    QDateTime cursor;
    if (spec.hasSeconds) {
        cursor = from.addSecs(1);
    } else {
        cursor = QDateTime(from.date(), QTime(from.time().hour(), from.time().minute(), 0)).addSecs(60);
    }

    // 5 年上限：足够覆盖 2 月 29 日这类低频日期，也保证「永不触发」能及时返回
    const QDateTime limit = from.addYears(5);
    const std::set<int> &secs = spec.second.values; // 5 段时是内部补的 {0}

    while (out.size() < count && cursor <= limit) {
        const QDate date = cursor.date();
        if (spec.mon.values.count(date.month()) == 0) {
            // 月不匹配：直接跳到下月 1 号零点（必须 addMonths，
            // 先跳到次日再取"本月 1 号"会倒退回当月月初、死循环）
            cursor = QDateTime(QDate(date.year(), date.month(), 1).addMonths(1), QTime(0, 0, 0));
            continue;
        }
        if (!detail::dayMatches(spec, date)) {
            cursor = QDateTime(date.addDays(1), QTime(0, 0, 0));
            continue;
        }

        // 当天：枚举时/分/秒的匹配组合（集合已去重升序）
        const int curH = cursor.time().hour();
        const int curM = cursor.time().minute();
        const int curS = cursor.time().second();
        for (int h : spec.hour.values) {
            if (out.size() >= count)
                break;
            if (h < curH)
                continue;
            for (int m : spec.minute.values) {
                if (out.size() >= count)
                    break;
                if (h == curH && m < curM)
                    continue;
                for (int sec : secs) {
                    if (out.size() >= count)
                        break;
                    if (h == curH && m == curM && sec < curS)
                        continue;
                    const QDateTime t(date, QTime(h, m, sec));
                    if (t < cursor)
                        continue;
                    out.append(t);
                }
            }
        }
        // 当天扫完（或已凑满），推进到次日零点
        cursor = QDateTime(date.addDays(1), QTime(0, 0, 0));
    }
    return out;
}

// ------------------------------------------------------------ 可视化字段编辑
// 面板「点选设置」用的状态机：每个字段五种模式，互转规则见 buildFieldRaw / classifyField。

/// 字段编辑模式（与面板单选按钮顺序一致）。
enum class FieldMode { Every, Period, Range, Values, Custom };

/// 一个字段的可视化编辑状态。面板存 6 份，按下标 0..5 对齐 FieldType。
struct FieldEdit
{
    FieldMode mode = FieldMode::Every;
    int periodFrom = 0;   ///< Period：起点
    int periodStep = 1;   ///< Period：步长
    int rangeFrom = 0;    ///< Range：起点
    int rangeTo = 0;      ///< Range：终点
    std::set<int> values; ///< Values：选中的具体值（std::set 天然升序去重）
    QString custom;       ///< Custom：用户直写的字段原文
};

/// 编辑器单位词（「每隔 5 分钟」里的"分钟"），与描述措辞共用一套量词。
inline QString fieldUnitWord(FieldType type)
{
    return detail::unitWord(type);
}

/// 编辑器「每个」模式的说明词（「每分钟」「每天」……）。
inline QString fieldEveryWord(FieldType type)
{
    return detail::everyWord(type);
}

/// 值网格按钮文字：分/秒/时补零两位方便扫列，周给中文名，其余直接数字。
inline QString valueButtonText(FieldType type, int v)
{
    if (type == FieldType::DayOfWeek)
        return dowName(v);
    if (type == FieldType::Second || type == FieldType::Minute || type == FieldType::Hour)
        return QStringLiteral("%1").arg(v, 2, 10, QChar('0'));
    return QString::number(v);
}

/// 相对时间文案（运行列表右侧的「15 小时后」）。负值按 0 处理。
inline QString relTimeText(qint64 secs)
{
    if (secs < 0)
        secs = 0;
    if (secs < 3600)
        return QStringLiteral("%1 分钟后").arg(qMax<qint64>(1, secs / 60));
    if (secs < 86400)
        return QStringLiteral("%1 小时后").arg(secs / 3600);
    return QStringLiteral("%1 天后").arg(secs / 86400);
}

/// 编辑状态 → 字段原文。失败写 error 并返回空串。
/// 约定：
///   Every  → "*"
///   Period → 起点等于字段最小值时写 "*/n"（更常见的写法），否则 "from/n"
///   Range  → "a-b"；Values → "a,b,c"（升序）；Custom → 用户原文（trim）
/// 最后统一过 parseField 校验并归一（小写、去空白），返回值保证能被 parse 吃下。
inline QString buildFieldRaw(const FieldEdit &e, FieldType type, QString *error = nullptr)
{
    int vmin = 0, vmax = 0;
    fieldRange(type, &vmin, &vmax);

    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return QString();
    };

    QString raw;
    switch (e.mode) {
    case FieldMode::Every:
        raw = QStringLiteral("*");
        break;
    case FieldMode::Period:
        if (e.periodStep < 1)
            return fail(QStringLiteral("步长必须大于 0"));
        if (e.periodFrom < vmin || e.periodFrom > vmax)
            return fail(QStringLiteral("起点超出范围（%1~%2）").arg(vmin).arg(vmax));
        raw = (e.periodFrom == vmin)
                  ? QStringLiteral("*/%1").arg(e.periodStep)
                  : QStringLiteral("%1/%2").arg(e.periodFrom).arg(e.periodStep);
        break;
    case FieldMode::Range:
        if (e.rangeFrom > e.rangeTo)
            return fail(QStringLiteral("起点大于终点"));
        if (e.rangeFrom < vmin || e.rangeTo > vmax)
            return fail(QStringLiteral("超出范围（%1~%2）").arg(vmin).arg(vmax));
        raw = QStringLiteral("%1-%2").arg(e.rangeFrom).arg(e.rangeTo);
        break;
    case FieldMode::Values:
        if (e.values.empty())
            return fail(QStringLiteral("至少选一个值"));
        {
            QStringList vs;
            for (int v : e.values) {
                if (v < vmin || v > vmax)
                    return fail(QStringLiteral("选中的值超出范围（%1~%2）").arg(vmin).arg(vmax));
                vs << QString::number(v);
            }
            raw = vs.join(QLatin1Char(','));
        }
        break;
    case FieldMode::Custom:
        raw = e.custom.trimmed();
        if (raw.isEmpty())
            return fail(QStringLiteral("这一段不能为空"));
        break;
    }

    // 兜底统一校验（Custom 内容尤其需要），归一后返回保证可解析
    const FieldSpec f = parseField(raw, type);
    if (!f.valid) {
        if (error)
            *error = f.error;
        return {};
    }
    return f.raw;
}

/// 解析结果 → 编辑状态（表达式回填可视化编辑器用）。归类规则：
///   "*" / "?" / "*/1"          → Every
///   "*/n"（n>1）                → Period(字段最小值, n)
///   "a/n"（openEnded）          → Period(a, n)
///   "a-b"（无步长）              → Range(a, b)
///   纯单值列表（"0"、"1,15"）   → Values
///   其余（"a-b/n"、混杂列表等） → Custom（原文保留，交给用户自己改）
inline FieldEdit classifyField(const FieldSpec &f, FieldType type)
{
    FieldEdit e;
    if (!f.valid) {
        e.mode = FieldMode::Custom;
        e.custom = f.raw;
        return e;
    }

    int vmin = 0, vmax = 0;
    fieldRange(type, &vmin, &vmax);

    if (f.wildcard && f.segments.size() == 1 && f.segments.first().step <= 1) {
        e.mode = FieldMode::Every;
        return e;
    }
    if (f.segments.size() == 1) {
        const Segment &s = f.segments.first();
        if (s.wildcard && s.step > 1) {
            e.mode = FieldMode::Period;
            e.periodFrom = vmin;
            e.periodStep = s.step;
            return e;
        }
        if (s.openEnded && s.step > 1) {
            e.mode = FieldMode::Period;
            e.periodFrom = s.lo;
            e.periodStep = s.step;
            return e;
        }
        if (s.isRange && !s.openEnded && s.step <= 1) {
            e.mode = FieldMode::Range;
            e.rangeFrom = s.lo;
            e.rangeTo = s.hi;
            return e;
        }
        if (!s.isRange && !s.wildcard && s.step <= 1) {
            e.mode = FieldMode::Values;
            e.values.insert(s.lo);
            return e;
        }
    }

    bool allSimple = true;
    for (const Segment &s : f.segments) {
        if (s.wildcard || s.isRange || s.step > 1) {
            allSimple = false;
            break;
        }
    }
    if (allSimple) {
        e.mode = FieldMode::Values;
        for (const Segment &s : f.segments)
            e.values.insert(s.lo);
        return e;
    }

    e.mode = FieldMode::Custom;
    e.custom = f.raw;
    return e;
}

} // namespace cron

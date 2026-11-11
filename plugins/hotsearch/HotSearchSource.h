#pragma once

// ---------------------------------------------------------------------------
// 热搜插件的「纯逻辑层」：榜单数据源表、接口响应解析、热度数值格式化、标签配色。
//
// 为什么单独拆一个 header：AGENTS.md 规则 2 要求交付前有实际跑过的验证，而仓库
// 不保留测试工程。把解析/格式化做成不依赖 QWidget、也不依赖 QNetworkAccessManager
// 的 inline 函数后，一个只链 Qt6::Core 的临时程序就能 include 本文件做断言，
// 喂真实抓下来的 JSON 验证字段差异，跑完即删。
//
// 数据源分两类，字段完全不同，所以解析器也分两个：
//   1) 聚合实例 DailyHotApi（hot.baiwumm.com）：{code, msg, data:[{title, hot, url, ...}]}，
//      微博 / 知乎 / 抖音 / B 站 / 头条 / 36 氪 / 少数派 / 掘金都走它，字段基本一致；
//   2) 百度官方榜单接口（top.baidu.com/api/board）：{success, data:{cards:[{component,
//      content:[...]}]}，条目里标题字段是 query（PC）或 word（移动），热度是 hotScore。
// 百度之所以优先走官方接口：它是唯一不需要登录、不需要签名就能匿名取到的官方榜；
// 聚合实例只是别人的公益服务器，随时可能停，只当百度的备胎。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace hotsearch {

/// 一个榜单数据源。三个 URL 的分工不能混：
///   apiUrl      主接口（能匿名取到 JSON 的那个）
///   fallbackUrl 主接口失败时的备胎，空表示没有备胎，直接报错
///   siteUrl     网页版榜单，接口全挂时给用户一个「自己去看」的出口
struct Source
{
    QString id;   ///< 稳定标识，仅用于日志与调试
    QString name; ///< 下拉框里的展示名
    QString apiUrl;
    QString fallbackUrl;
    QString siteUrl;
};

/// 榜单清单。顺序即下拉框顺序：微博在最前（「今日热搜」最常看的就是它）。
/// 聚合实例的所有接口都在 hot.baiwumm.com 下，路径就是平台 id，改版时只需改这里。
inline const QVector<Source> &sources()
{
    static const QVector<Source> list = {
        {QStringLiteral("weibo"), QStringLiteral("微博热搜"),
         QStringLiteral("https://hot.baiwumm.com/api/weibo"), QString(),
         QStringLiteral("https://s.weibo.com/top/summary")},
        {QStringLiteral("baidu"), QStringLiteral("百度热搜"),
         // 官方接口优先；注意 platform=pc 才有 hotScore，wise 版没有热度值
         QStringLiteral("https://top.baidu.com/api/board?platform=pc&tab=realtime"),
         QStringLiteral("https://hot.baiwumm.com/api/baidu"),
         QStringLiteral("https://top.baidu.com/board?tab=realtime")},
        {QStringLiteral("zhihu"), QStringLiteral("知乎热榜"),
         QStringLiteral("https://hot.baiwumm.com/api/zhihu"), QString(),
         QStringLiteral("https://www.zhihu.com/hot")},
        {QStringLiteral("douyin"), QStringLiteral("抖音热榜"),
         QStringLiteral("https://hot.baiwumm.com/api/douyin"), QString(),
         QStringLiteral("https://www.douyin.com/hot")},
        {QStringLiteral("bilibili"), QStringLiteral("B 站热门"),
         QStringLiteral("https://hot.baiwumm.com/api/bilibili"), QString(),
         QStringLiteral("https://www.bilibili.com/v/popular/rank/all")},
        {QStringLiteral("toutiao"), QStringLiteral("今日头条"),
         QStringLiteral("https://hot.baiwumm.com/api/toutiao"), QString(),
         QStringLiteral("https://www.toutiao.com/trending/")},
        {QStringLiteral("36kr"), QStringLiteral("36 氪热榜"),
         QStringLiteral("https://hot.baiwumm.com/api/36kr"), QString(),
         QStringLiteral("https://36kr.com/hot-list/catalog")},
        {QStringLiteral("sspai"), QStringLiteral("少数派"),
         QStringLiteral("https://hot.baiwumm.com/api/sspai"), QString(),
         QStringLiteral("https://sspai.com/")},
        {QStringLiteral("juejin"), QStringLiteral("掘金热榜"),
         QStringLiteral("https://hot.baiwumm.com/api/juejin"), QString(),
         QStringLiteral("https://juejin.cn/hot")},
    };
    return list;
}

/// 一条热搜。desc 只有部分平台给（B 站的视频简介、少数派的摘要），面板只在
/// 悬停提示里用它，不占行高。hot 保留原始文本（可能是 "1607675" 这样的数字串，
/// 也有的平台直接给 "8.2 万热度"），格式化交给 formatHot()。
struct Item
{
    QString title;
    QString desc;
    QString hot;
    QString label; ///< 微博的「热 / 新 / 沸 / 荐」；多数平台没有，留空
    QString url;   ///< 点条目要在浏览器里打开的地址
};

/// 解析结果。error 非空即失败（此时 items 可能是空数组），面板据 error 给出原因，
/// 不允许静默给一个空列表 —— 用户分不清「今天没热搜」和「接口挂了」。
struct Result
{
    QVector<Item> items;
    QString error;
};

/// JSON 标量统一转成文本。有的聚合实例把 hot 写成数字、有的写成字符串，
/// 这里抹平差异，避免上层到处判 isString/isDouble。
inline QString jsonText(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble()) {
        // 热度都是整数，但 QJsonValue 只有 double；转 qint64 再输出可去掉 ".000000"
        const double raw = value.toDouble();
        if (raw >= -9.0e15 && raw <= 9.0e15)
            return QString::number(static_cast<qint64>(raw));
        return QString::number(raw, 'g', 12);
    }
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return QString();
}

/// 接口里可能出现 "code": 200 或 "code": "200"，两种都认。
/// 缺字段时按 0 处理（DailyHotApi 成功时一定给 200，百度接口则根本没有 code）。
inline int jsonCode(const QJsonObject &root)
{
    const QJsonValue code = root.value(QStringLiteral("code"));
    if (code.isDouble())
        return code.toInt();
    if (code.isString())
        return code.toString().toInt();
    return 0;
}

/// 热度数值 → 人的读法：1607675 → "160.8万"，29810000 → "2981万"，8.6 亿 → "8.6亿"。
/// 非数字（已经是「8.2 万热度」这种文案）原样返回，不猜单位。
inline QString formatHot(const QString &raw)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return QString();

    bool ok = false;
    const qint64 value = text.toLongLong(&ok);
    if (!ok || value < 0)
        return text;

    // 一位小数就够；".0" 要抹掉，否则「2981.0万」看着像精度很高其实没有
    auto oneDecimal = [](double v) {
        QString s = QString::number(v, 'f', 1);
        if (s.endsWith(QLatin1String(".0")))
            s.chop(2);
        return s;
    };

    if (value >= 100000000LL)
        return oneDecimal(value / 100000000.0) + QStringLiteral("亿");
    if (value >= 10000LL)
        return oneDecimal(value / 10000.0) + QStringLiteral("万");
    return QString::number(value);
}

/// 标签 → 主题令牌名（不含 '@'）。返回的令牌交给 applyStyle，不在这里拼颜色字符串，
/// 否则深色模式下这几枚小标签会变成深底深字。
/// 语义：沸 / 爆 = 正在炸的（红），热 / 新 / 荐 = 值得点（橙），其余一律强调色。
inline QString labelToken(const QString &label)
{
    if (label.contains(QStringLiteral("沸")) || label.contains(QStringLiteral("爆")))
        return QStringLiteral("danger");
    if (label.contains(QStringLiteral("热")) || label.contains(QStringLiteral("新"))
        || label.contains(QStringLiteral("荐")) || label.contains(QStringLiteral("首发")))
        return QStringLiteral("warn");
    return QStringLiteral("accent");
}

/// 排名色：前三名给颜色，其余用最弱的灰。前三用三种语义色而不是同一色深浅，
/// 是因为浅色底上纯靠透明度区分第 1 和第 3 几乎看不出来。
inline QString rankToken(int rank)
{
    if (rank == 1)
        return QStringLiteral("danger");
    if (rank == 2)
        return QStringLiteral("warn");
    if (rank == 3)
        return QStringLiteral("accent");
    return QStringLiteral("text-faint");
}

/// 解析聚合实例（DailyHotApi）的响应。
/// 返回 {code, msg, data:[{title, desc, hot, label, url, mobileUrl}]}；
/// url 缺失时退到 mobileUrl（个别平台的 url 为空、只有移动端地址）。
inline Result parseAggregated(const QByteArray &body)
{
    Result out;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        out.error = QStringLiteral("返回内容不是合法 JSON（%1）").arg(parseError.errorString());
        return out;
    }
    if (!doc.isObject()) {
        out.error = QStringLiteral("返回内容不是 JSON 对象");
        return out;
    }

    const QJsonObject root = doc.object();
    const QJsonValue data = root.value(QStringLiteral("data"));
    if (!data.isArray()) {
        // 失败时实例会给 {"code":500,"msg":"..."}：把 msg 透出去比「数据为空」有用得多
        const QString msg = jsonText(root.value(QStringLiteral("msg"))).trimmed();
        out.error = msg.isEmpty()
                        ? QStringLiteral("返回里没有 data 列表（接口可能改版）")
                        : QStringLiteral("%1（code %2）").arg(msg).arg(jsonCode(root));
        return out;
    }

    const QJsonArray array = data.toArray();
    out.items.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject obj = value.toObject();

        Item item;
        item.title = jsonText(obj.value(QStringLiteral("title"))).trimmed();
        if (item.title.isEmpty())
            continue;   // 标题都没有的条目没有意义，直接跳过而不是留一行空白
        item.desc = jsonText(obj.value(QStringLiteral("desc"))).trimmed();
        item.hot = jsonText(obj.value(QStringLiteral("hot")));
        item.label = jsonText(obj.value(QStringLiteral("label"))).trimmed();
        item.url = jsonText(obj.value(QStringLiteral("url"))).trimmed();
        if (item.url.isEmpty())
            item.url = jsonText(obj.value(QStringLiteral("mobileUrl"))).trimmed();
        out.items.append(item);
    }

    if (out.items.isEmpty())
        out.error = QStringLiteral("接口返回了 0 条（可能被限流或榜单结构变了）");
    return out;
}

/// 解析百度官方榜单接口。结构比聚合接口深两层，且卡片 component 会变：
///   PC   platform=pc   → cards[i].component == "hotList"，content 直接是条目数组，
///                        条目里是 query / hotScore / desc / url；
///   Wise platform=wise → cards[i].component == "tabTextList"，content 是「分区」，
///                        条目在分区自己的 content 里，标题字段叫 word，没有热度。
/// 两种都接住：先按 component 认，认不出就按元素形状猜（对象里有 content 数组就是分区）。
inline Result parseBaiduBoard(const QByteArray &body)
{
    Result out;

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        out.error = QStringLiteral("返回内容不是合法 JSON（%1）").arg(parseError.errorString());
        return out;
    }

    const QJsonObject root = doc.object();
    if (!root.value(QStringLiteral("success")).toBool(false) && !root.contains(QStringLiteral("data"))) {
        const QString error = jsonText(root.value(QStringLiteral("error"))).trimmed();
        out.error = error.isEmpty() ? QStringLiteral("百度榜单接口没有返回 data")
                                    : QStringLiteral("百度榜单接口报错：%1").arg(error);
        return out;
    }

    const QJsonArray cards = root.value(QStringLiteral("data"))
                                 .toObject()
                                 .value(QStringLiteral("cards"))
                                 .toArray();
    if (cards.isEmpty()) {
        out.error = QStringLiteral("百度榜单接口返回的 cards 为空（可能改版）");
        return out;
    }

    for (const QJsonValue &cardValue : cards) {
        const QJsonObject card = cardValue.toObject();
        for (const QJsonValue &sectionValue : card.value(QStringLiteral("content")).toArray()) {
            // 分区：{content:[条目...]}；直接把条目当元素的情况也归到 else 分支
            QJsonArray entries;
            if (sectionValue.isObject()
                && sectionValue.toObject().value(QStringLiteral("content")).isArray()) {
                entries = sectionValue.toObject().value(QStringLiteral("content")).toArray();
            } else if (sectionValue.isObject()) {
                entries = QJsonArray{sectionValue};
            }

            for (const QJsonValue &entryValue : entries) {
                const QJsonObject entry = entryValue.toObject();

                Item item;
                item.title = jsonText(entry.value(QStringLiteral("query"))).trimmed();
                if (item.title.isEmpty())
                    item.title = jsonText(entry.value(QStringLiteral("word"))).trimmed();
                if (item.title.isEmpty())
                    continue;
                item.desc = jsonText(entry.value(QStringLiteral("desc"))).trimmed();
                item.hot = jsonText(entry.value(QStringLiteral("hotScore")));
                item.url = jsonText(entry.value(QStringLiteral("url"))).trimmed();
                if (item.url.isEmpty())
                    item.url = jsonText(entry.value(QStringLiteral("rawUrl"))).trimmed();
                // 百度的 hotTag 是图片等级（"3" 之类），不是可读标签，不往 label 里塞
                out.items.append(item);
            }
        }
    }

    if (out.items.isEmpty())
        out.error = QStringLiteral("百度榜单接口解析出 0 条（结构可能变了）");
    return out;
}

/// 按「请求的是哪个主机」挑解析器。放在纯逻辑层是为了让临时测试能覆盖分流规则：
/// 把 top.baidu.com 的响应错喂给聚合解析器，会得到「没有 data 列表」这种误导性错误。
inline Result parseResponse(const QString &url, const QByteArray &body)
{
    if (url.contains(QLatin1String("top.baidu.com"), Qt::CaseInsensitive))
        return parseBaiduBoard(body);
    return parseAggregated(body);
}

} // namespace hotsearch

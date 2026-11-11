#pragma once

// ---------------------------------------------------------------------------
// 金融行情插件的「纯逻辑」部分：数据结构、请求地址拼装、回包解析、数值格式化。
//
// 这里刻意不发网络请求、不碰任何界面控件，只依赖 QtCore —— 于是可以像
// plugins/calc/ExprEval.h 那样，用一个临时工程直接 include 它做断言测试
// （见 AGENTS.md「测试」一节：跑通、贴结果、删临时工程）。
//
// 数据源：东方财富的公开行情接口（详见 README 的「金融行情」章节）
//   1) push2 ulist.np   —— 股票 / 指数 / ETF / 黄金期货等「有代码的品种」，
//      支持一次问多只（secid 形如 "1.600519"：市场号.代码），回包字段是数字缩写；
//   2) fundmobapi       —— 场外基金（没有交易所代码，只能按 6 位基金代码查），
//      回包里日线净值/估值都是字符串，得自己转。
//
// 为什么两套而不是一套：场外基金根本不存在「实时价格」，它只有每日净值，
// 盘中有基金公司给的估算值。把它硬塞进股票接口只会拿到错误的 0 元报价。
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
#include <QUrlQuery>
#include <QVector>

#include <cmath>

namespace finance {

/// 品种类型。只用于「搜索结果里给个标签」和「自选里认不认得出这是什么」，
/// 行情走哪个接口是按 id 前缀判断的（见 isFundId），不依赖这个字段。
enum class Kind
{
    Stock,
    Index,
    Etf,
    Futures,
    Fund,     ///< 场外基金（id 形如 "fund:000001"）
    Other,
};

/// 一个可查询的品种。id 是唯一键：
///   - 行情类："<市场号>.<代码>"，如 "1.600519"、"118.AU9999"
///   - 场外基金："fund:<6 位代码>"，如 "fund:000001"
struct Symbol
{
    QString id;
    QString name;
    Kind kind = Kind::Other;
};

/// 一次查询的结果。数值都用 double，格式化交给 formatXxx()。
struct Quote
{
    QString id;
    QString name;
    double price = 0.0;          ///< 最新价 / 净值 / 估值
    double change = 0.0;         ///< 涨跌额（基金是相对上一日净值的差）
    double changePercent = 0.0;  ///< 涨跌幅，单位是百分号里的数（-0.41 表示 -0.41%）
    int decimals = 2;            ///< 价格保留几位小数（基金 4 位，指数 2 位，COMEX 1 位）
    QString note;                ///< 补充说明：净值日期 / 累计净值 / 估值时间
    bool hasPrice = false;       ///< 停牌、退市、接口没给数时为 false，界面显示 "—"
};

// ---------------------------------------------------------------- 基础判定

/// 场外基金走另一套接口，用 id 前缀区分（自选项只存 id，能自解释）。
inline bool isFundId(const QString &id)
{
    return id.startsWith(QLatin1String("fund:"));
}

/// "fund:000001" -> "000001"；行情 id 原样返回（调用方自己判断要不要用）。
inline QString fundCodeOf(const QString &id)
{
    return isFundId(id) ? id.mid(5) : id;
}

/// 品种标签，搜索结果里用。
inline QString kindLabel(Kind kind)
{
    switch (kind) {
    case Kind::Stock:   return QStringLiteral("A股");
    case Kind::Index:   return QStringLiteral("指数");
    case Kind::Etf:     return QStringLiteral("ETF");
    case Kind::Futures: return QStringLiteral("期货");
    case Kind::Fund:    return QStringLiteral("基金");
    case Kind::Other:   break;
    }
    return QStringLiteral("行情");
}

/// 市场号 → 市场名。数字来自东方财富的 secid 约定，只列本项目用得到的几个；
/// 认不出来时返回空串，界面就只显示代码，不硬编一个假名字。
inline QString marketLabel(const QString &id)
{
    const int dot = id.indexOf(QLatin1Char('.'));
    if (dot <= 0)
        return QString();
    const int market = id.left(dot).toInt();
    switch (market) {
    case 0:   return QStringLiteral("深市");
    case 1:   return QStringLiteral("沪市");
    case 90:  return QStringLiteral("板块");
    case 101: return QStringLiteral("COMEX");
    case 102: return QStringLiteral("NYMEX");
    case 113: return QStringLiteral("上期所");
    case 114: return QStringLiteral("大商所");
    case 115: return QStringLiteral("郑商所");
    case 118: return QStringLiteral("上海金交所");
    default:  break;
    }
    return QString();
}

/// 自选项目的持久化编码："<id>|<名字>"。名字里出现 '|' 的概率可以忽略，
/// 解析侧仍然只按第一个 '|' 切，多出来的部分原样并回名字里。
inline QString encodeSymbol(const Symbol &symbol)
{
    return symbol.id + QLatin1Char('|') + symbol.name;
}

/// 解码失败（空串 / 没有分隔符 / id 为空）返回 false，调用方直接跳过这一条，
/// 不要因为配置文件被手改坏就整块自选都不显示。
inline bool decodeSymbol(const QString &text, Symbol *symbol)
{
    if (!symbol)
        return false;
    const int sep = text.indexOf(QLatin1Char('|'));
    if (sep <= 0)
        return false;
    symbol->id = text.left(sep);
    symbol->name = text.mid(sep + 1);
    symbol->kind = isFundId(symbol->id) ? Kind::Fund : Kind::Other;
    return !symbol->id.isEmpty();
}

// ---------------------------------------------------------------- 数值格式化

/// 价格：固定小数位。涨跌幅用两位，价格用接口给的精度。
inline QString formatPrice(double value, int decimals)
{
    return QString::number(value, 'f', qBound(0, decimals, 6));
}

/// 带正负号的涨跌额：+5.99 / -5.99 / 0.00。零不写 "+"，避免读出「涨了」的错觉。
inline QString formatSigned(double value, int decimals)
{
    const QString text = QString::number(std::fabs(value), 'f', qBound(0, decimals, 6));
    if (value > 0.0)
        return QStringLiteral("+") + text;
    if (value < 0.0)
        return QStringLiteral("-") + text;
    return text;
}

/// 带正负号的百分比：+0.92% / -0.41%。
inline QString formatPercent(double percent)
{
    const QString text = QString::number(std::fabs(percent), 'f', 2);
    if (percent > 0.0)
        return QStringLiteral("+") + text + QStringLiteral("%");
    if (percent < 0.0)
        return QStringLiteral("-") + text + QStringLiteral("%");
    return text + QStringLiteral("%");
}

/// 涨跌方向：1 涨 / -1 跌 / 0 平。界面靠它选红色/绿色/灰字
/// （A 股惯例：涨红跌绿，与欧美相反）。
inline int direction(double value)
{
    if (value > 0.0)
        return 1;
    if (value < 0.0)
        return -1;
    return 0;
}

// ---------------------------------------------------------------- 请求地址

/// 行情批量查询地址。secids 形如 {"1.600519", "118.AU9999"}。
/// 字段含义（东方财富的缩写，别改错）：
///   f1 小数位 / f2 最新价 / f3 涨跌幅×100 / f4 涨跌额 / f6 成交额 /
///   f12 代码 / f13 市场号 / f14 名称 / f18 昨收
inline QUrl buildMarketQuoteUrl(const QStringList &secids)
{
    QUrl url(QStringLiteral("https://push2.eastmoney.com/api/qt/ulist.np/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("secids"), secids.join(QLatin1Char(',')));
    query.addQueryItem(QStringLiteral("fields"),
                       QStringLiteral("f1,f2,f3,f4,f6,f12,f13,f14,f18"));
    url.setQuery(query);
    return url;
}

/// 场外基金批量查询地址。plat/appType/product/Version/deviceid 是接口要求的
/// 「我是移动端 App」标识，缺了会返回空数组（不是报错，更难查）。
inline QUrl buildFundQuoteUrl(const QStringList &codes)
{
    QUrl url(QStringLiteral("https://fundmobapi.eastmoney.com/FundMNewApi/FundMNFInfo"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("pageIndex"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("pageSize"), QString::number(qMax(1, codes.size())));
    query.addQueryItem(QStringLiteral("plat"), QStringLiteral("Android"));
    query.addQueryItem(QStringLiteral("appType"), QStringLiteral("ttjj"));
    query.addQueryItem(QStringLiteral("product"), QStringLiteral("EFund"));
    query.addQueryItem(QStringLiteral("Version"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("deviceid"), QStringLiteral("cxk-tools"));
    query.addQueryItem(QStringLiteral("Fcodes"), codes.join(QLatin1Char(',')));
    url.setQuery(query);
    return url;
}

/// 品种搜索（股票 / 指数 / ETF / 期货），返回里有现成的 QuoteID 可直接当 id 用。
/// token 是该接口的公开固定值，不是我们的账号凭据。
inline QUrl buildSuggestUrl(const QString &keyword)
{
    QUrl url(QStringLiteral("https://searchapi.eastmoney.com/api/suggest/get"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("input"), keyword);
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("14"));
    query.addQueryItem(QStringLiteral("token"),
                       QStringLiteral("D43BF722C8E33BDC906FB84D85E326E8"));
    query.addQueryItem(QStringLiteral("count"), QStringLiteral("8"));
    url.setQuery(query);
    return url;
}

/// 场外基金搜索（按代码或名称）。
inline QUrl buildFundSuggestUrl(const QString &keyword)
{
    QUrl url(QStringLiteral("https://fundsuggest.eastmoney.com/FundSearch/api/FundSearchAPI.ashx"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("m"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("key"), keyword);
    url.setQuery(query);
    return url;
}

// ---------------------------------------------------------------- 回包解析

namespace detail {

/// 接口里的数字可能被写成字符串（停牌时甚至是 "-"），统一走这里取值，
/// 取不到就给 fallback —— 绝不抛异常，也绝不把 NaN 传到界面上。
inline double numberValue(const QJsonValue &value, double fallback = 0.0)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().trimmed().toDouble(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

inline int intValue(const QJsonValue &value, int fallback)
{
    if (value.isDouble())
        return value.toInt();
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        return ok ? parsed : fallback;
    }
    return fallback;
}

/// 解析 JSON 并把「不是 JSON」与「JSON 但结构不对」区分开，方便状态行给出可读原因。
inline bool parseDocument(const QByteArray &payload, QJsonDocument *document, QString *error)
{
    QJsonParseError parseError{};
    *document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("返回内容不是合法 JSON：%1").arg(parseError.errorString());
        return false;
    }
    if (!document->isObject()) {
        if (error)
            *error = QStringLiteral("返回结构异常（顶层不是对象）");
        return false;
    }
    return true;
}

} // namespace detail

/// 解析行情批量回包。data 为 null（所有 secid 都不认识）时返回 true 且 out 为空。
inline bool parseMarketQuotes(const QByteArray &payload, QVector<Quote> *out, QString *error)
{
    if (out)
        out->clear();

    QJsonDocument document;
    if (!detail::parseDocument(payload, &document, error))
        return false;

    const QJsonObject root = document.object();
    const int rc = detail::intValue(root.value(QStringLiteral("rc")), 0);
    if (rc != 0) {
        if (error)
            *error = QStringLiteral("接口返回错误码 rc=%1").arg(rc);
        return false;
    }

    const QJsonValue dataValue = root.value(QStringLiteral("data"));
    if (!dataValue.isObject())
        return true;   // data:null 表示没有可返回的品种，不算失败

    const QJsonArray diff = dataValue.toObject().value(QStringLiteral("diff")).toArray();
    for (const QJsonValue &item : diff) {
        const QJsonObject object = item.toObject();

        Quote quote;
        const QString code = object.value(QStringLiteral("f12")).toString();
        const int market = detail::intValue(object.value(QStringLiteral("f13")), -1);
        if (code.isEmpty() || market < 0)
            continue;
        quote.id = QStringLiteral("%1.%2").arg(market).arg(code);
        quote.name = object.value(QStringLiteral("f14")).toString();
        // f1 是小数位数：股票 2 位、指数 2 位、COMEX 贵金属 1 位、债 3 位
        quote.decimals = detail::intValue(object.value(QStringLiteral("f1")), 2);
        if (quote.decimals < 0 || quote.decimals > 6)
            quote.decimals = 2;

        const double scale = std::pow(10.0, quote.decimals);
        const double rawPrice = detail::numberValue(object.value(QStringLiteral("f2")));
        quote.price = rawPrice / scale;
        quote.change = detail::numberValue(object.value(QStringLiteral("f4"))) / scale;
        // f3 是「百分比 × 100」，-41 表示 -0.41%
        quote.changePercent = detail::numberValue(object.value(QStringLiteral("f3"))) / 100.0;
        quote.hasPrice = rawPrice > 0.0;

        out->append(quote);
    }
    return true;
}

/// 解析场外基金回包。
///
/// 盘中有基金公司估算值时（GSZ/GSZZL 非空）优先用估算值，note 里标明是估值；
/// 否则用当日净值，涨跌幅取 NAVCHGRT，涨跌额由
/// 「净值 - 上一日净值」反推：上一日 = 今日 / (1 + r/100)。
inline bool parseFundQuotes(const QByteArray &payload, QVector<Quote> *out, QString *error)
{
    if (out)
        out->clear();

    QJsonDocument document;
    if (!detail::parseDocument(payload, &document, error))
        return false;

    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("Success")).toBool(true)) {
        if (error)
            *error = QStringLiteral("基金接口返回失败：%1")
                         .arg(root.value(QStringLiteral("ErrMsg")).toString(
                             QStringLiteral("未知原因")));
        return false;
    }

    const QJsonArray items = root.value(QStringLiteral("Datas")).toArray();
    for (const QJsonValue &item : items) {
        const QJsonObject object = item.toObject();

        Quote quote;
        const QString code = object.value(QStringLiteral("FCODE")).toString();
        if (code.isEmpty())
            continue;
        quote.id = QStringLiteral("fund:") + code;
        quote.name = object.value(QStringLiteral("SHORTNAME")).toString();
        quote.decimals = 4;

        const QString navText = object.value(QStringLiteral("NAV")).toString();
        const QString accText = object.value(QStringLiteral("ACCNAV")).toString();
        const double nav = detail::numberValue(object.value(QStringLiteral("NAV")));
        const double navChangePercent =
            detail::numberValue(object.value(QStringLiteral("NAVCHGRT")));

        const double estimate = detail::numberValue(object.value(QStringLiteral("GSZ")));
        const QString estimateTime = object.value(QStringLiteral("GZTIME")).toString();

        if (estimate > 0.0 && !estimateTime.isEmpty()) {
            quote.price = estimate;
            quote.changePercent = detail::numberValue(object.value(QStringLiteral("GSZZL")));
            quote.change = estimate - nav;
            quote.note = QStringLiteral("估值 %1 · 净值 %2").arg(estimateTime, navText);
            quote.hasPrice = true;
        } else if (nav > 0.0) {
            quote.price = nav;
            quote.changePercent = navChangePercent;
            // 反推上一日净值；-100% 会把分母变成 0，这里直接判掉
            const double previous = (navChangePercent > -100.0)
                                        ? nav / (1.0 + navChangePercent / 100.0)
                                        : nav;
            quote.change = nav - previous;
            quote.note = QStringLiteral("净值 %1 · 累计 %2")
                             .arg(object.value(QStringLiteral("PDATE")).toString(), accText);
            quote.hasPrice = true;
        } else {
            quote.note = QStringLiteral("暂无净值");
        }

        out->append(quote);
    }
    return true;
}

/// 解析品种搜索回包。只保留能在行情接口里查到的品种（板块、债券这类
/// QuoteID 也能查，但和「自选行情」关系不大，一律过滤掉，免得列表全是噪音）。
inline bool parseSuggest(const QByteArray &payload, QVector<Symbol> *out, QString *error)
{
    if (out)
        out->clear();

    QJsonDocument document;
    if (!detail::parseDocument(payload, &document, error))
        return false;

    const QJsonObject table =
        document.object().value(QStringLiteral("QuotationCodeTable")).toObject();
    const QJsonArray items = table.value(QStringLiteral("Data")).toArray();

    for (const QJsonValue &item : items) {
        const QJsonObject object = item.toObject();
        const QString id = object.value(QStringLiteral("QuoteID")).toString();
        const QString name = object.value(QStringLiteral("Name")).toString();
        if (id.isEmpty() || name.isEmpty())
            continue;

        const QString classify = object.value(QStringLiteral("Classify")).toString();
        Kind kind = Kind::Other;
        if (classify == QLatin1String("AStock"))
            kind = Kind::Stock;
        else if (classify == QLatin1String("Index"))
            kind = Kind::Index;
        else if (classify == QLatin1String("Fund"))
            kind = Kind::Etf;   // 交易所场内基金，走行情接口
        else if (classify == QLatin1String("UniversalFutures"))
            kind = Kind::Futures;
        else
            continue;           // 板块 / 债券 / 港股美股等：本工具不接

        out->append(Symbol{id, name, kind});
    }
    return true;
}

/// 解析场外基金搜索回包。
///
/// 注意这个接口是「基金 + 股票」混合搜索：搜「茅台」或「600519」时 Datas 里会冒出
/// 一条 CATEGORYDESC="沪市"、FundBaseInfo=null 的**股票**记录（CODE 就是 600519）。
/// 早期版本没过滤，结果把「贵州茅台」当成场外基金存进自选，查行情时永远拿不到数据。
/// 判定标准：CATEGORYDESC 必须是「基金」，且 FundBaseInfo 是个对象。
inline bool parseFundSuggest(const QByteArray &payload, QVector<Symbol> *out, QString *error)
{
    if (out)
        out->clear();

    QJsonDocument document;
    if (!detail::parseDocument(payload, &document, error))
        return false;

    const QJsonArray items = document.object().value(QStringLiteral("Datas")).toArray();
    for (const QJsonValue &item : items) {
        const QJsonObject object = item.toObject();
        if (object.value(QStringLiteral("CATEGORYDESC")).toString() != QStringLiteral("基金"))
            continue;
        if (!object.value(QStringLiteral("FundBaseInfo")).isObject())
            continue;

        const QString code = object.value(QStringLiteral("CODE")).toString();
        const QString name = object.value(QStringLiteral("NAME")).toString();
        if (code.isEmpty() || name.isEmpty())
            continue;
        out->append(Symbol{QStringLiteral("fund:") + code, name, Kind::Fund});
    }
    return true;
}

} // namespace finance

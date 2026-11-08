#pragma once

// ---------------------------------------------------------------------------
// 翻译插件的纯逻辑层：语言表、源语言检测、各引擎的请求构造与响应解析、长文本分段。
//
// 刻意不依赖任何 QWidget / Q_OBJECT，只用 Qt Core 的值类型：
//   * AGENTS.md 规则 2 要求临时断言程序能直接 include 本文件验证，而不是复制代码去测；
//   * 网络响应形态差异（四家服务的 JSON 完全不一样）全部在这里抹平，面板只认
//     ParseResult，换服务或加签名接口时不用动界面。
//
// 四个数据源与它们的硬约束（均为实测，改动前请重新确认）：
//   1. 有道演示接口 POST https://aidemo.youdao.com/trans
//      - 表单 q/from/to，无需 appKey；成功 JSON 形如
//        {"errorCode":"0","translation":["你好世界"],"l":"en2zh-CHS"}
//      - errorCode 103 = 文本过长，411 = 同 IP 请求过于频繁，102 = 语言方向不支持；
//      - 实测单次文本上限在 800~1200 字符之间，且连续请求很容易触发 411，
//        所以面板必须节流 + 失败自动降级，不能把它当唯一数据源。
//   2. 搜狗网页接口 POST https://fanyi.sogou.com/api/transpc/text/result
//      - 需要先 GET https://fanyi.sogou.com/ 建立会话：响应 HTML 的
//        window.__INITIAL_STATE__ 里带 secretCode 与 uuid，同时下发 cookie；
//        没有 cookie 时接口固定返回 translate.errorCode = "s10"。
//      - 表单 s = md5(from + to + text + secretCode)（小写十六进制），
//        另需 client=pc / fr=browser_pc / needQc=1 / uuid / exchange=false；
//      - 成功时 data.translate.dit 是译文、data.detect.detect 是源语言码；
//        status=-1 多为翻译方向不支持或请求过密，0.1 秒连发会偶发命中。
//      - 实测繁体中文不是它的目标语言（会把原文原样返回），源=目标时它会
//        擅自把方向反过来（en→en 返回中文），英→韩稳定返回 status=-1。
//        这三类都必须在发请求前拦掉或按失败降级，绝不能把错译文展示给用户。
//   3. MyMemory GET https://api.mymemory.translated.net/get
//      - q + langpair=<from>|<to>，免费匿名档单次 q 上限 500 字符，
//        超限返回 responseStatus=403 且 translatedText 里直接塞错误文案；
//      - 它的 langpair=autodetect|xx 会把原文原样返回（实测 "Hello world"），
//        因此源语言一律由本地 detectSource() 推断后传明确码，不用它的自动检测。
//   4. Google gtx GET https://translate.googleapis.com/translate_a/single
//      - 返回嵌套数组 [[["译文","原文",...]],null,"en"]，不是对象；
//      - 国内网络通常不可达，仅作为海外用户的最后兜底，超时要短。
//
// 统一分段上限取 400 字符：同时满足有道（<1000）、搜狗（实测 1200 仍可用）
// 与 MyMemory（500）的限制，也避免一段失败时重译整篇。所有常量都集中在本文件，
// 面板不写死服务细节。
//
// 腾讯翻译君不在数据源里：官方 API 已于 2025-09-15 关闭，现存的
// api.fanyi.qq.com/api/v1/* 需要 TCaptcha 滑块验证码换 Token，transmart.qq.com
// 需要登录态，桌面端都无法匿名自动调用。不再提供「腾讯网页」跳转按钮（应要求移除）。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QChar>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtGlobal>   // qBound / qMin（本文件是纯 header，不能靠调用方间接包含）
#include <QUrl>
#include <QUrlQuery>

namespace translate {

/// 单个文本片段 + 它与下一段之间的分隔符。
/// tail 只保留「换行」或「空格」两种语义：译文标点与原文不一定对应，
/// 原样回贴标点反而会出现两段译文之间多一个孤立句号。
struct Chunk
{
    QString text;   ///< 待翻译片段（已去掉两端空白）
    QString tail;   ///< 片段之后的空白（"\n" 或 " "），最后一段为空
};

/// 一种语言在四个服务里的代码。展示名用中文，顺序即界面下拉顺序。
/// sogou 为空串表示该语言**不能作为搜狗的目标语言**（繁体中文实测会原样返回）。
struct Language
{
    QString label;      ///< 界面文案，如「中文」
    QString youdao;     ///< 有道码，如 zh-CHS
    QString sogou;      ///< 搜狗码，如 zh-CHS；空 = 目标方向不支持
    QString mymemory;   ///< MyMemory 码，如 zh-CN
    QString google;     ///< Google 码，如 zh-CN
};

/// 语言表。函数内静态局部变量由 C++11 起保证线程安全初始化；
/// 面板每次构造都会读它，但状态全部放在面板实例里（宿主不缓存面板，禁止全局状态）。
inline const QList<Language> &languages()
{
    static const QList<Language> table = {
        {QStringLiteral("中文"),     QStringLiteral("zh-CHS"), QStringLiteral("zh-CHS"), QStringLiteral("zh-CN"), QStringLiteral("zh-CN")},
        {QStringLiteral("英语"),     QStringLiteral("en"),     QStringLiteral("en"),     QStringLiteral("en"),    QStringLiteral("en")},
        {QStringLiteral("日语"),     QStringLiteral("ja"),     QStringLiteral("ja"),     QStringLiteral("ja"),    QStringLiteral("ja")},
        {QStringLiteral("韩语"),     QStringLiteral("ko"),     QStringLiteral("ko"),     QStringLiteral("ko"),    QStringLiteral("ko")},
        {QStringLiteral("法语"),     QStringLiteral("fr"),     QStringLiteral("fr"),     QStringLiteral("fr"),    QStringLiteral("fr")},
        {QStringLiteral("德语"),     QStringLiteral("de"),     QStringLiteral("de"),     QStringLiteral("de"),    QStringLiteral("de")},
        {QStringLiteral("俄语"),     QStringLiteral("ru"),     QStringLiteral("ru"),     QStringLiteral("ru"),    QStringLiteral("ru")},
        {QStringLiteral("西班牙语"), QStringLiteral("es"),     QStringLiteral("es"),     QStringLiteral("es"),    QStringLiteral("es")},
        {QStringLiteral("葡萄牙语"), QStringLiteral("pt"),     QStringLiteral("pt"),     QStringLiteral("pt"),    QStringLiteral("pt")},
        {QStringLiteral("意大利语"), QStringLiteral("it"),     QStringLiteral("it"),     QStringLiteral("it"),    QStringLiteral("it")},
        {QStringLiteral("阿拉伯语"), QStringLiteral("ar"),     QStringLiteral("ar"),     QStringLiteral("ar"),    QStringLiteral("ar")},
        // 搜狗网页端只提供简体中文，繁体目标会原样返回原文：留空让 buildRequest 判为不支持并降级。
        {QStringLiteral("繁体中文"), QStringLiteral("zh-CHT"), QString(),               QStringLiteral("zh-TW"), QStringLiteral("zh-TW")},
    };
    return table;
}

/// 自动降级顺序即枚举顺序：有道（质量与速度最好）→ 搜狗（国内第二家、需会话握手）
/// → MyMemory（稳定）→ Google（海外兜底）。
/// 面板「引擎」下拉多出的「自动」项不等于这里的值，而是「从这个顺序挨个试」。
enum class Engine
{
    Youdao = 0,
    Sogou,
    MyMemory,
    Google,
    Count
};

inline QString engineName(Engine engine)
{
    switch (engine) {
    case Engine::Youdao:   return QStringLiteral("有道");
    case Engine::Sogou:    return QStringLiteral("搜狗");
    case Engine::MyMemory: return QStringLiteral("MyMemory");
    case Engine::Google:   return QStringLiteral("Google");
    default:               return QStringLiteral("未知服务");
    }
}

/// 有道 errorCode → 用户能看懂的中文原因。
/// 这些码来自有道智云公共错误码表；不认识的码原样带出，绝不吞掉。
inline QString youdaoErrorMessage(const QString &code)
{
    if (code == QStringLiteral("102")) return QStringLiteral("不支持这组语言方向");
    if (code == QStringLiteral("103")) return QStringLiteral("文本过长");
    if (code == QStringLiteral("108")) return QStringLiteral("服务密钥无效");
    if (code == QStringLiteral("113")) return QStringLiteral("待翻译文本为空");
    if (code == QStringLiteral("302")) return QStringLiteral("服务翻译查询失败");
    if (code == QStringLiteral("411")) return QStringLiteral("请求过于频繁，请稍后再试");
    return QStringLiteral("服务返回错误码 %1").arg(code);
}

/// 把 JSON 里的标量统一读成字符串。
/// 实测有道的 errorCode 有时是字符串 "0"、有时是数字 411；MyMemory 的
/// responseStatus 同理。只按 isString() 读会把数字形态全判为失败。
inline QString scalarText(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(value.toInt());
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return QString();
}

/// 解码最常见的 HTML 实体。MyMemory 的记忆库译文里偶尔带 &amp; / &#39;，
/// 不处理就会把 "Tom &amp; Jerry" 原样显示给用户。
/// 只认白名单、且 &amp; 最后替换：&amp;lt; 应还原成字面量 "&lt;" 而不是 "<"。
inline QString decodeEntities(QString text)
{
    text.replace(QStringLiteral("&lt;"),   QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"),   QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    text.replace(QStringLiteral("&apos;"), QStringLiteral("'"));
    text.replace(QStringLiteral("&#39;"),  QStringLiteral("'"));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"),  QStringLiteral("&"));
    return text;
}

/// 解析一次响应的结果。detectedFrom 是服务回传的源语言码（有道 l / MyMemory
/// detectedLanguage / Google 数组第 3 项），只用于状态栏展示，不参与后续请求。
struct ParseResult
{
    bool    ok = false;
    QString text;
    QString detectedFrom;
    QString error;
    /// 搜狗会话/签名失效：面板应清掉缓存的 secretCode，下次翻译重新握手。
    bool    sessionExpired = false;
};

/// 解析有道演示接口响应。
inline ParseResult parseYoudao(const QByteArray &body)
{
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // 演示接口被网关拦下时会返回 HTML，统一按「非 JSON」报错
        result.error = QStringLiteral("服务返回了无法解析的内容");
        return result;
    }

    const QJsonObject obj = doc.object();
    const QString code = scalarText(obj.value(QStringLiteral("errorCode")));
    if (!code.isEmpty() && code != QStringLiteral("0")) {
        result.error = youdaoErrorMessage(code);
        return result;
    }

    const QJsonArray parts = obj.value(QStringLiteral("translation")).toArray();
    QStringList lines;
    for (const QJsonValue &part : parts) {
        const QString line = part.toString().trimmed();
        if (!line.isEmpty())
            lines.append(line);
    }
    if (lines.isEmpty()) {
        result.error = QStringLiteral("服务没有返回译文");
        return result;
    }

    result.ok = true;
    result.text = lines.join(QLatin1Char('\n'));
    // l 形如 "zh-CHS2en"：切在第一个 '2' 之前取源语言。
    // 不能简单 split("2")，语言码本身不含 '2'，但用 indexOf 语义更明确。
    const QString direction = obj.value(QStringLiteral("l")).toString();
    const int sep = direction.indexOf(QLatin1Char('2'));
    if (sep > 0)
        result.detectedFrom = direction.left(sep);
    return result;
}

/// 解析 MyMemory 响应。
inline ParseResult parseMyMemory(const QByteArray &body)
{
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("服务返回了无法解析的内容");
        return result;
    }

    const QJsonObject obj = doc.object();
    const QString status = scalarText(obj.value(QStringLiteral("responseStatus")));
    if (!status.isEmpty() && status != QStringLiteral("200")) {
        const QString detail = obj.value(QStringLiteral("responseDetails")).toString().trimmed();
        result.error = detail.isEmpty()
                         ? QStringLiteral("服务返回状态 %1").arg(status)
                         : detail;
        return result;
    }

    const QJsonObject data = obj.value(QStringLiteral("responseData")).toObject();
    QString text = decodeEntities(data.value(QStringLiteral("translatedText")).toString().trimmed());
    if (text.isEmpty()) {
        result.error = QStringLiteral("服务没有返回译文");
        return result;
    }
    // 超限时接口会把错误文案直接塞进 translatedText，别把它当译文展示
    if (data.value(QStringLiteral("translatedText")).toString().contains(
            QStringLiteral("QUERY LENGTH LIMIT EXCEEDED"))) {
        result.error = QStringLiteral("片段超长，请缩短后重试");
        return result;
    }

    result.ok = true;
    result.text = text;
    result.detectedFrom = data.value(QStringLiteral("detectedLanguage")).toString();
    return result;
}

/// 解析 Google gtx 的嵌套数组：[[["译文","原文",...], ...], null, "en", ...]。
inline ParseResult parseGoogle(const QByteArray &body)
{
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        result.error = QStringLiteral("服务返回了无法解析的内容");
        return result;
    }

    const QJsonArray root = doc.array();
    const QJsonArray sentences = root.isEmpty() ? QJsonArray{}
                                               : root.at(0).toArray();
    QStringList lines;
    for (const QJsonValue &sentence : sentences) {
        const QJsonArray fields = sentence.toArray();
        if (!fields.isEmpty()) {
            const QString line = fields.at(0).toString().trimmed();
            if (!line.isEmpty())
                lines.append(line);
        }
    }
    if (lines.isEmpty()) {
        result.error = QStringLiteral("服务没有返回译文");
        return result;
    }

    result.ok = true;
    result.text = lines.join(QString());   // gtx 的分句自带标点/空格，直接相连
    if (root.size() > 2)
        result.detectedFrom = root.at(2).toString();
    return result;
}

/// 搜狗会话参数。接口要求先 GET 首页拿 cookie，并从 SSR 的初始化数据里取
/// secretCode（签名盐）与 uuid（请求标识）；两者都是**本次会话**的值，不落盘。
struct SogouSession
{
    bool    ok = false;
    QString secretCode;
    QString uuid;
    QString error;
};

inline QUrl sogouHandshakeUrl()
{
    return QUrl(QStringLiteral("https://fanyi.sogou.com/"));
}

/// 搜狗首页按 UA 决定是否下发 SSR 配置，接口也可能把会话与 UA 关联：
/// 握手和翻译两处必须用同一个桌面浏览器 UA，别一处自定义一处默认。
inline QByteArray sogouUserAgent()
{
    return QByteArray("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36");
}

/// 从搜狗首页 HTML 解析会话参数。页面把配置放在 window.__INITIAL_STATE__ 里，
/// 形如 "secretCode":109984457,"uuid":"420260ce-...."。用正则而不是整段 JSON 解析：
/// 这个值只是一大段 SSR 状态里的两个字段，整段里还混着未转义的 HTML，解析必然失败。
inline SogouSession parseSogouHandshake(const QByteArray &body)
{
    SogouSession session;
    const QString page = QString::fromUtf8(body);

    const QRegularExpression secretRe(
        QStringLiteral("\"secretCode\"\\s*:\\s*\"?(\\d+)\"?"));
    const QRegularExpression uuidRe(
        QStringLiteral("\"uuid\"\\s*:\\s*\"([^\"]+)\""));
    const QRegularExpressionMatch secretMatch = secretRe.match(page);
    const QRegularExpressionMatch uuidMatch = uuidRe.match(page);

    if (!secretMatch.hasMatch() || !uuidMatch.hasMatch()) {
        session.error = QStringLiteral("未能取得搜狗会话参数（页面结构可能已变更）");
        return session;
    }
    session.ok = true;
    session.secretCode = secretMatch.captured(1);
    session.uuid = uuidMatch.captured(1);
    return session;
}

/// 搜狗表单签名：md5(from + to + text + secretCode) 的小写十六进制。
/// 拼接顺序来自网页端 JS 的 Q.a.cal(from+to+text+secretCode)，cal 就是 MD5；
/// 四个参数必须与 POST 表单里的值逐字一致，服务端会按同样规则重算比对。
inline QString sogouSignature(const QString &from, const QString &to,
                              const QString &text, const QString &secretCode)
{
    const QByteArray data = (from + to + text + secretCode).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
}

/// 搜狗实测不支持的方向。英→韩稳定返回 status=-1（中→韩、韩→中正常），
/// 繁体中文不是它的目标语言（sogou 码为空），源=目标时它会擅自反转方向。
/// 提前拦掉可以省掉一次必然失败的请求与 1 秒节流等待，也让降级原因更可读。
inline QString sogouDirectionError(const QString &from, const QString &to)
{
    // 文案不带服务名：面板会把 engineName() 作为前缀拼成「搜狗：xxx」。
    if (to.isEmpty())
        return QStringLiteral("不支持译为繁体中文");
    if (from == to)
        return QStringLiteral("源语言与目标语言相同，会改变译文方向");
    if (from == QLatin1String("en") && to == QLatin1String("ko"))
        return QStringLiteral("不支持英语译为韩语");
    return QString();
}

/// 解析搜狗网页接口响应：data.translate.dit 是译文，data.detect.detect 是源语言码。
inline ParseResult parseSogou(const QByteArray &body)
{
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("服务返回了无法解析的内容");
        return result;
    }

    const QJsonObject obj = doc.object();
    const QString status = scalarText(obj.value(QStringLiteral("status")));
    if (!status.isEmpty() && status != QStringLiteral("0")) {
        result.error = (status == QStringLiteral("-1"))
                         ? QStringLiteral("服务不支持这组语言方向或请求过于频繁")
                         : QStringLiteral("服务返回状态 %1").arg(status);
        return result;
    }

    const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
    const QJsonObject translated = data.value(QStringLiteral("translate")).toObject();
    const QString code = scalarText(translated.value(QStringLiteral("errorCode")));
    if (code == QStringLiteral("s10")) {
        result.error = QStringLiteral("会话或签名校验失败（需重新建立会话）");
        result.sessionExpired = true;
        return result;
    }
    if (!code.isEmpty() && code != QStringLiteral("0")) {
        result.error = QStringLiteral("服务返回错误码 %1").arg(code);
        return result;
    }

    const QString text = decodeEntities(
        translated.value(QStringLiteral("dit")).toString().trimmed());
    if (text.isEmpty()) {
        result.error = QStringLiteral("服务没有返回译文");
        return result;
    }

    result.ok = true;
    result.text = text;
    result.detectedFrom = data.value(QStringLiteral("detect")).toObject()
                              .value(QStringLiteral("detect")).toString();
    return result;
}

/// 按引擎分派解析，面板只调这一个入口。
inline ParseResult parseResponse(Engine engine, const QByteArray &body)
{
    switch (engine) {
    case Engine::Youdao:   return parseYoudao(body);
    case Engine::Sogou:    return parseSogou(body);
    case Engine::MyMemory: return parseMyMemory(body);
    case Engine::Google:   return parseGoogle(body);
    default:
        return ParseResult{};
    }
}

/// 一次翻译需要的全部语言参数。
/// 把「同一语言在四家服务里代码不同」这件事收在一个结构里，
/// makeParams() 只算一次，面板按段循环时不再做表查找。
struct RequestParams
{
    QString text;
    QString fromYoudao;     ///< "Auto" 或明确码（有道服务端检测更准，自动模式优先用它）
    QString fromSogou;      ///< "auto" 或明确码（搜狗服务端检测对拉丁语系更准）
    QString detectedSogou;  ///< 本地检测出的源语言码，用于判断「auto 是否与目标同语言」
    QString fromMyMemory;
    QString fromGoogle;
    QString toYoudao;
    QString toSogou;
    QString toMyMemory;
    QString toGoogle;
    QString sogouSecret;    ///< 面板握手后填入，本文件不缓存任何会话状态
    QString sogouUuid;      ///< 同上
};

/// 构造一次翻译的语言参数。
/// @param fromIndex 源语言在 languages() 中的下标，-1 = 自动检测；
/// @param toIndex   目标语言下标，必须合法（界面保证）；
/// @param detectedIndex 本地检测出的源语言下标，自动模式下给 MyMemory/Google/搜狗用。
inline RequestParams makeParams(const QString &text, int fromIndex, int toIndex,
                                int detectedIndex)
{
    const QList<Language> &table = languages();
    const Language to = table.at(qBound(0, toIndex, table.size() - 1));
    const int effectiveFrom = (fromIndex >= 0) ? fromIndex : detectedIndex;
    const Language from = table.at(qBound(0, effectiveFrom, table.size() - 1));

    RequestParams params;
    params.text = text;
    // 自动模式交给有道自己的检测器：实测 from=Auto&to=zh-CHS / to=en 都可用，
    // 而 from=Auto&to=ja 会返回 102（不支持方向），那种情况由面板依次降级到搜狗等备用服务。
    params.fromYoudao   = (fromIndex >= 0) ? from.youdao : QStringLiteral("Auto");
    // 自动模式交给搜狗服务端检测：本地检测把法语/德语等拉丁脚本一律回落英语，
    // 传明确码反而会翻错；同语言与不支持的方向由 buildRequest 提前拦掉。
    params.fromSogou    = (fromIndex >= 0) ? from.sogou : QStringLiteral("auto");
    params.detectedSogou = table.at(qBound(0, detectedIndex, table.size() - 1)).sogou;
    params.fromMyMemory = from.mymemory;
    params.fromGoogle   = (fromIndex >= 0) ? from.google : QStringLiteral("auto");
    params.toYoudao     = to.youdao;
    params.toSogou      = to.sogou;
    params.toMyMemory   = to.mymemory;
    params.toGoogle     = to.google;
    return params;
}

/// 生成某个引擎的实际 HTTP 请求。
/// isPost=true 时调用方要把 body 作为表单 POST 出去；否则直接 GET url。
/// QUrlQuery 的 toString(FullyEncoded) 是必须的：MyMemory 的 langpair 里带 '|'，
/// 未编码的 '|' 在部分代理/服务端会被当成参数分隔符，导致 403。
struct RequestSpec
{
    QUrl        url;
    QByteArray  body;
    QByteArray  referer;    ///< 个别网页接口校验来源站，由面板设成 Referer 头
    QByteArray  userAgent;  ///< 非空 = 覆盖面板的默认 UA（搜狗需要浏览器 UA）
    QString     error;      ///< 非空 = 这家服务当前方向不可用，面板据此直接降级
    bool        isPost = false;
};

inline RequestSpec buildRequest(Engine engine, const RequestParams &params)
{
    RequestSpec spec;
    switch (engine) {
    case Engine::Youdao: {
        spec.url = QUrl(QStringLiteral("https://aidemo.youdao.com/trans"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"), params.text);
        query.addQueryItem(QStringLiteral("from"), params.fromYoudao);
        query.addQueryItem(QStringLiteral("to"), params.toYoudao);
        spec.body = query.toString(QUrl::FullyEncoded).toUtf8();
        spec.isPost = true;
        break;
    }
    case Engine::Sogou: {
        // 手动把「繁体中文」选作源语言时 sogou 码为空，按官方别名回落到简体。
        const QString fromValue = params.fromSogou.isEmpty()
                                      ? QStringLiteral("zh-CHS") : params.fromSogou;
        const QString &toValue = params.toSogou;
        // auto 模式要拿本地检测码参与「同语言 / 不支持方向」判断，避免服务端把方向反过来。
        const QString effectiveFrom = (fromValue == QLatin1String("auto"))
                                          ? params.detectedSogou : fromValue;
        spec.error = sogouDirectionError(effectiveFrom, toValue);
        if (!spec.error.isEmpty())
            return spec;
        if (params.sogouSecret.isEmpty() || params.sogouUuid.isEmpty()) {
            spec.error = QStringLiteral("尚未取得会话参数");
            return spec;
        }

        spec.url = QUrl(QStringLiteral("https://fanyi.sogou.com/api/transpc/text/result"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("from"), fromValue);
        query.addQueryItem(QStringLiteral("to"), toValue);
        query.addQueryItem(QStringLiteral("text"), params.text);
        query.addQueryItem(QStringLiteral("client"), QStringLiteral("pc"));
        query.addQueryItem(QStringLiteral("fr"), QStringLiteral("browser_pc"));
        query.addQueryItem(QStringLiteral("needQc"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("s"),
                           sogouSignature(fromValue, toValue, params.text,
                                          params.sogouSecret));
        query.addQueryItem(QStringLiteral("uuid"), params.sogouUuid);
        query.addQueryItem(QStringLiteral("exchange"), QStringLiteral("false"));
        spec.body = query.toString(QUrl::FullyEncoded).toUtf8();
        spec.isPost = true;
        spec.referer = "https://fanyi.sogou.com/text";
        spec.userAgent = sogouUserAgent();
        break;
    }
    case Engine::MyMemory: {
        QUrl url(QStringLiteral("https://api.mymemory.translated.net/get"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"), params.text);
        query.addQueryItem(QStringLiteral("langpair"),
                           params.fromMyMemory + QLatin1Char('|') + params.toMyMemory);
        url.setQuery(query);
        spec.url = url;
        break;
    }
    case Engine::Google: {
        QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
        query.addQueryItem(QStringLiteral("sl"), params.fromGoogle);
        query.addQueryItem(QStringLiteral("tl"), params.toGoogle);
        query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
        query.addQueryItem(QStringLiteral("q"), params.text);
        url.setQuery(query);
        spec.url = url;
        break;
    }
    default:
        break;
    }
    return spec;
}

/// 是否适合作为分段切点：空白或常见句读。
/// 中文没有词间空格，只靠空白切会硬断在词中间，所以把标点也算进来。
inline bool isBreakChar(QChar c)
{
    if (c.isSpace())
        return true;
    const char16_t u = c.unicode();
    switch (u) {
    case u'.': case u',': case u';': case u':': case u'!': case u'?':
    case 0x3002:  // 。
    case 0xFF0C:  // ，
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0x3001:  // 、
        return true;
    default:
        return false;
    }
}

/// 把长文本切成不超过 maxChars 的片段，尽量落在句子/词边界，并记住段间空白。
/// 空行会生成一个 text 为空的占位片段：面板跳过它的请求，但拼接时仍要还回换行，
/// 否则用户原文里的分段会在译文里消失。
inline QList<Chunk> splitText(const QString &text, int maxChars = 400)
{
    QList<Chunk> out;
    if (maxChars < 20)
        maxChars = 20;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int li = 0; li < lines.size(); ++li) {
        const QString &line = lines.at(li);
        int pos = 0;
        bool produced = false;

        while (pos < line.size()) {
            int take = qMin(line.size() - pos, maxChars);
            if (pos + take < line.size()) {
                // 只在窗口后半段找切点：找太靠前会切出大量极短的片段，
                // 既浪费请求次数又更容易撞上接口限流。
                int cut = -1;
                for (int i = pos + take - 1; i >= pos + take / 2; --i) {
                    if (isBreakChar(line.at(i))) {
                        cut = i + 1;
                        break;
                    }
                }
                if (cut > pos)
                    take = cut - pos;
            }

            Chunk chunk;
            chunk.text = line.mid(pos, take).trimmed();
            pos += take;
            // 吃掉切点后的空白；同一行后面还有内容时用一个空格连回译文
            while (pos < line.size() && line.at(pos).isSpace())
                ++pos;
            if (pos < line.size())
                chunk.tail = QLatin1Char(' ');
            out.append(chunk);
            produced = true;
        }

        if (!produced) {
            Chunk blank;
            blank.tail = QLatin1Char('\n');
            out.append(blank);
        } else if (li + 1 < lines.size()) {
            out.last().tail = QLatin1Char('\n');
        }
    }
    return out;
}

/// 把各片段的译文按原分段结构拼回整篇。
/// translations 与 chunks 一一对应；缺失的片段按空串处理（不让某段失败拖垮整篇布局）。
inline QString joinChunks(const QList<Chunk> &chunks, const QStringList &translations)
{
    QString out;
    for (int i = 0; i < chunks.size(); ++i) {
        out += translations.value(i);
        const QString tail = chunks.at(i).tail;
        if (tail.contains(QLatin1Char('\n')))
            out += QLatin1Char('\n');
        else if (!tail.isEmpty())
            out += QLatin1Char(' ');
    }
    return out.trimmed();
}

/// 本地源语言检测，返回 languages() 的下标。
///
/// 为什么不用服务端的自动检测：MyMemory 的 autodetect 模式实测会把原文原样返回，
/// 有道对部分「Auto→小语种」方向直接报 102；本地按 Unicode 区段判断既零延迟，
/// 又能给三家服务都传上明确的源语言码。只覆盖常见脚本，判不出来一律回落英语。
inline int detectSource(const QString &text)
{
    int han = 0, kana = 0, hangul = 0, cyrillic = 0, arabic = 0, latin = 0;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0x31F0 && u <= 0x31FF)) {
            ++kana;                                    // 平假名 / 片假名（含扩展）
        } else if ((u >= 0xAC00 && u <= 0xD7AF) || (u >= 0x1100 && u <= 0x11FF)) {
            ++hangul;                                  // 谚文音节 / 字母
        } else if ((u >= 0x3400 && u <= 0x4DBF) || (u >= 0x4E00 && u <= 0x9FFF)
                   || (u >= 0xF900 && u <= 0xFAFF)) {
            ++han;                                     // CJK 统一表字 / 兼容表意文字
        } else if (u >= 0x0400 && u <= 0x04FF) {
            ++cyrillic;
        } else if (u >= 0x0600 && u <= 0x06FF) {
            ++arabic;
        } else if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')) {
            ++latin;
        }
    }

    // 日文混汉字、韩文夹汉字都很常见：假名/谚文优先于汉字判定。
    // 中文里夹几个英文单词不应改变结论，所以汉字只要出现就压过拉丁计数。
    if (kana > 0)    return 2;   // 日语
    if (hangul > 0)  return 3;   // 韩语
    if (han > 0)     return 0;   // 中文（简体；繁体由用户手动选）
    if (cyrillic > latin) return 6;  // 俄语
    if (arabic > latin)   return 10; // 阿拉伯语
    return 1;                        // 英语 / 其他拉丁脚本，回落英语
}

/// 服务回传的码 → 界面展示名。四家服务的码不统一，认不出的原样显示，不强行猜。
inline QString languageLabelForCode(const QString &code)
{
    if (code.isEmpty())
        return QString();
    for (const Language &lang : languages()) {
        if (code == lang.youdao || code == lang.google || code == lang.mymemory
            || (!lang.sogou.isEmpty() && code == lang.sogou))
            return lang.label;
    }
    return code;
}

} // namespace translate

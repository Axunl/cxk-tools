#pragma once

// ---------------------------------------------------------------------------
// JWT（JSON Web Token）编解码纯逻辑。
//
// 职责与边界：
//   * 只做 JWS 紧凑序列化的解码、HMAC 系列（HS256/384/512）的生成与签名校验，
//     不碰网络、不碰 UI —— 方便按 AGENTS.md 规则 2 写临时断言程序直接 include 验证；
//   * RS/ES 等非对称算法只解码展示，不校验（需要公钥与完整 JOSE 实现，超出本地
//     小工具的定位）；alg=none 可生成（仅供调试）但从不判定为"校验通过"；
//   * 全部函数为纯函数（无全局状态），面板可以放心多实例（主窗口 + 独立窗口并存）。
//
// 关键实现约束（踩坑记录，改动前先读）：
//   * base64url 是 RFC 4648 §5 的变体：字母表用 `-_` 而非 `+/`，且通常省略 `=` 填充；
//   * 签名覆盖的是「原始两段文本」headerB64 + "." + payloadB64，不是重新序列化后的
//     JSON —— 所以 DecodeResult 必须保留 signingInput，校验时绝不能重新编码；
//   * QJsonObject 序列化会**按键名排序**（Qt 6.8 实测 `{"b":1,"a":2}` → `{"a":2,"b":1}`），
//     因此编码侧不能走「parse → toJson(Compact)」，否则打乱用户字段顺序、生成的令牌
//     与 jwt.io 等工具对不上。这里改用 compactJson()：先用 QJsonParseError 校验，
//     再按字符扫描只删除字符串外的空白（保序、确定性输出）。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMessageAuthenticationCode>
#include <QString>
#include <QStringList>

namespace jwtlogic {

// ---------------------------------------------------------------- 基础编解码

/// base64url 编码（RFC 4648 §5）：`-_` 字母表、去掉 `=` 填充。
inline QString base64UrlEncode(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding
                                             | QByteArray::OmitTrailingEquals));
}

/// base64url 解码。兼容两种写法：省略填充的（标准 JWT）与带 `=` 填充的。
/// ok 置 false 的情况：非法字符、长度 %4 == 1（无法补齐的残段）。
/// 空文本解码为空数组且 ok=true —— alg=none 的签名段就是空串。
inline QByteArray base64UrlDecode(const QString &text, bool *ok = nullptr)
{
    QByteArray bytes = text.toLatin1();
    // 非 Latin-1 字符经 toLatin1() 变成 '?'，不在字母表内，最终会被 fromBase64 拒绝。
    bytes.replace('-', '+');
    bytes.replace('_', '/');

    const int rem = bytes.size() % 4;
    if (rem == 1) {          // base64 不可能存在长度 %4 == 1 的合法串
        if (ok)
            *ok = false;
        return {};
    }
    if (rem != 0)
        bytes.append(QByteArray(4 - rem, '='));

    const auto decoded = QByteArray::fromBase64Encoding(bytes, QByteArray::AbortOnBase64DecodingErrors);
    if (ok)
        *ok = (decoded.decodingStatus == QByteArray::Base64DecodingStatus::Ok);
    return decoded.decoded;
}

// ---------------------------------------------------------------- JSON 压缩

/// 校验并「保序压缩」一段 JSON 文本：
///   1. 用 QJsonParseError 保证语法合法且是对象（JWT 的 header/payload 必须是对象）；
///   2. 只删掉字符串外的空白（空格/换行/制表），键序与字符串内容原样保留。
///
/// 为什么不直接 QJsonDocument::toJson(Compact)：它会按键名排序（见文件头注释），
/// 压缩后的文本就是最终要 base64url 进令牌的字节，必须与用户书写顺序一致才可复现。
///
/// 成功返回 true 并把结果写入 *json；失败返回 false 并在 *error 写可读原因。
inline bool compactJson(const QString &text, QString *json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error) {
            *error = QStringLiteral("JSON 解析失败（第 %1 字节附近）：%2")
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
        }
        return false;
    }
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("必须是 JSON 对象（以 { 开头、以 } 结尾）");
        return false;
    }

    QString out;
    out.reserve(text.size());
    bool inString = false;
    bool escaped = false;
    for (const QChar ch : text) {
        if (inString) {
            // 字符串内部的空白是内容，一律保留；转义引号不能终结字符串
            out.append(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
        } else if (ch == QLatin1Char('"')) {
            inString = true;
            out.append(ch);
        } else if (!ch.isSpace()) {
            out.append(ch);
        }
    }
    if (json)
        *json = out;
    return true;
}

// ---------------------------------------------------------------- 解码

/// 解码结果。signingInput 是签名覆盖的原文（两段 b64 用 "." 连接），
/// 校验签名必须用它，不能拿 headerText/payloadText 重新编码。
struct DecodeResult
{
    bool        ok = false;          ///< 整体是否解码成功
    QString     error;               ///< 失败原因（ok=false 时非空）
    QString     headerText;          ///< 格式化后的 Header JSON（展示用）
    QString     payloadText;         ///< 格式化后的 Payload JSON；非 JSON 对象时为解码原文
    QJsonObject header;              ///< Header 对象
    QJsonObject payload;             ///< Payload 对象（payloadIsObject 时有效）
    bool        payloadIsObject = false;
    QString     alg;                 ///< header.alg 原样取出（可能为空）
    QString     signingInput;        ///< "headerB64.payloadB64"，签名原文
    QString     signatureText;       ///< 第三段原始文本（base64url）
    QByteArray  signature;           ///< 第三段解码后的签名字节
};

/// 把 JSON 字节格式化成缩进文本（展示用）。
/// 注意：QJsonObject 会按键名排序，所以展示顺序可能与令牌内原始顺序不同 ——
/// 展示无所谓，签名校验走 signingInput，不受影响。
inline QString prettyJson(const QByteArray &jsonBytes)
{
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (doc.isNull())
        return QString::fromUtf8(jsonBytes);   // 非 JSON 就原样展示
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
}

/// 解码 JWS 紧凑序列化令牌："header.payload.signature" 三段，段内为 base64url。
/// 容忍粘贴时混入的空白/换行（先整体剔除再切分），段数不对或任一段非法都算失败。
inline DecodeResult decodeToken(const QString &token)
{
    DecodeResult r;

    // 原始令牌是单行文本，粘贴时可能被折行/带首尾空白 —— 先清掉再切分
    QString compact;
    compact.reserve(token.size());
    for (const QChar ch : token) {
        if (!ch.isSpace())
            compact.append(ch);
    }
    if (compact.isEmpty()) {
        r.error = QStringLiteral("令牌为空");
        return r;
    }

    const QStringList parts = compact.split(QLatin1Char('.'));
    if (parts.size() != 3) {
        r.error = QStringLiteral("JWT 应由 3 段组成（用 . 分隔），当前 %1 段").arg(parts.size());
        return r;
    }

    // ---- 第 1 段：Header（必须是 JSON 对象）
    bool segOk = false;
    const QByteArray headerBytes = base64UrlDecode(parts.at(0), &segOk);
    if (!segOk) {
        r.error = QStringLiteral("第 1 段（Header）不是合法的 base64url");
        return r;
    }
    QJsonParseError headerParse;
    const QJsonDocument headerDoc = QJsonDocument::fromJson(headerBytes, &headerParse);
    if (headerParse.error != QJsonParseError::NoError || !headerDoc.isObject()) {
        r.error = QStringLiteral("第 1 段（Header）解码后不是合法的 JSON 对象");
        return r;
    }
    r.header = headerDoc.object();
    r.headerText = prettyJson(headerBytes);
    r.alg = r.header.value(QStringLiteral("alg")).toString();

    // ---- 第 2 段：Payload（标准是 JSON 对象；容忍非 JSON，原样展示）
    const QByteArray payloadBytes = base64UrlDecode(parts.at(1), &segOk);
    if (!segOk) {
        r.error = QStringLiteral("第 2 段（Payload）不是合法的 base64url");
        return r;
    }
    QJsonParseError payloadParse;
    const QJsonDocument payloadDoc = QJsonDocument::fromJson(payloadBytes, &payloadParse);
    if (payloadParse.error == QJsonParseError::NoError && payloadDoc.isObject()) {
        r.payload = payloadDoc.object();
        r.payloadIsObject = true;
        r.payloadText = prettyJson(payloadBytes);
    } else {
        // 非对象 payload 不算解码失败：照样展示原文，方便看异常令牌
        r.payloadText = QString::fromUtf8(payloadBytes);
    }

    // ---- 第 3 段：签名（alg=none 时为空串，属合法形态）
    r.signatureText = parts.at(2);
    r.signature = base64UrlDecode(parts.at(2), &segOk);
    if (!segOk) {
        r.error = QStringLiteral("第 3 段（签名）不是合法的 base64url");
        return r;
    }

    r.signingInput = parts.at(0) + QLatin1Char('.') + parts.at(1);
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- 签名校验

/// 签名校验结论。
enum class VerifyResult
{
    Valid,        ///< 签名与密钥匹配（仅 HS*）
    Invalid,      ///< 签名不匹配：密钥不对，或令牌被篡改
    NeedSecret,   ///< HS* 算法但没填密钥
    Unsupported,  ///< 非 HS*（RS/ES/none 等），本地不支持校验
    BadToken,     ///< 令牌本身没解码成功
};

/// 恒定时间比较签名，避免按字节短路比较的时序侧信道。
/// （本地小工具其实用不上这么讲究，但 HMAC 校验顺手做对并不贵。）
inline bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return diff == 0;
}

/// alg 文本 → QCryptographicHash。不是 HS* 系列返回 false。
inline bool hmacAlgorithm(const QString &alg, QCryptographicHash::Algorithm *out)
{
    if (alg == QLatin1String("HS256")) {
        *out = QCryptographicHash::Sha256;
        return true;
    }
    if (alg == QLatin1String("HS384")) {
        *out = QCryptographicHash::Sha384;
        return true;
    }
    if (alg == QLatin1String("HS512")) {
        *out = QCryptographicHash::Sha512;
        return true;
    }
    return false;
}

/// 用 HMAC 对 signingInput 签名（生成与校验共用）。
inline QByteArray hmacSign(QCryptographicHash::Algorithm alg, const QByteArray &key,
                           const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, alg);
}

/// 校验签名。只认 HS256/384/512；其余算法明确返回 Unsupported 而不是含糊的失败。
inline VerifyResult verifyToken(const DecodeResult &decoded, const QString &secret)
{
    if (!decoded.ok)
        return VerifyResult::BadToken;

    if (decoded.alg.isEmpty())
        return VerifyResult::Unsupported;

    QCryptographicHash::Algorithm alg;
    if (!hmacAlgorithm(decoded.alg, &alg))
        return VerifyResult::Unsupported;   // 含 alg=none 与 RS/ES 系列

    if (secret.isEmpty())
        return VerifyResult::NeedSecret;

    const QByteArray expected = hmacSign(alg, secret.toUtf8(), decoded.signingInput.toLatin1());
    return constantTimeEquals(expected, decoded.signature) ? VerifyResult::Valid
                                                           : VerifyResult::Invalid;
}

/// 校验结论的展示文案（状态栏用），全部为可读中文。
inline QString verifyResultText(VerifyResult result, const QString &alg)
{
    switch (result) {
    case VerifyResult::Valid:
        return QStringLiteral("签名校验通过（%1）").arg(alg);
    case VerifyResult::Invalid:
        return QStringLiteral("签名校验失败：密钥不对或令牌被篡改（%1）").arg(alg);
    case VerifyResult::NeedSecret:
        return QStringLiteral("已解码（%1）：填入密钥可校验签名").arg(alg);
    case VerifyResult::Unsupported:
        if (alg == QLatin1String("none"))
            return QStringLiteral("alg=none：令牌无签名，任何人可伪造，不要用于鉴权");
        return QStringLiteral("已解码（%1）：本地仅支持校验 HS256/384/512 签名").arg(
            alg.isEmpty() ? QStringLiteral("alg 缺失") : alg);
    case VerifyResult::BadToken:
        return QStringLiteral("令牌不合法，无法校验");
    }
    return {};
}

// ---------------------------------------------------------------- 时间声明

/// 把 exp / iat / nbf 这类数值型时间声明格式化为本地时间文本。
/// 非数值（字符串日期等非标准写法）返回空串，由上层决定展示什么。
inline QString formatUnixTime(const QJsonValue &value)
{
    if (!value.isDouble())
        return {};
    // JSON 数字可能是 1.516239022E9 这种形式，统一按整数秒处理
    const qint64 secs = static_cast<qint64>(value.toDouble());
    return QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// ---------------------------------------------------------------- 编码

/// 编码结果。
struct EncodeResult
{
    bool    ok = false;
    QString error;    ///< 失败原因（ok=false 时非空）
    QString token;    ///< 生成的 JWS 紧凑序列化令牌
    QString alg;      ///< 实际使用的算法（从 Header.alg 读出）
};

/// 生成 JWT。
///   * headerJson / payloadJson 是用户编辑的 JSON 文本，经 compactJson() 校验并
///     **保序压缩**后直接作为令牌内容（所以同样的输入必然产出同样的令牌，可复现）；
///   * 签名算法以 Header 里的 "alg" 为准（JWT 语义如此），支持 HS256/384/512/none；
///   * HS* 需要非空密钥；none 会生成空签名段（调试用，状态栏要提示不安全）。
inline EncodeResult encodeToken(const QString &headerJson, const QString &payloadJson,
                                const QString &secret)
{
    EncodeResult r;

    QString headerText;
    QString err;
    if (!compactJson(headerJson, &headerText, &err)) {
        r.error = QStringLiteral("Header：%1").arg(err);
        return r;
    }
    QString payloadText;
    if (!compactJson(payloadJson, &payloadText, &err)) {
        r.error = QStringLiteral("Payload：%1").arg(err);
        return r;
    }

    // alg 从压缩后的 Header 里读，保证与令牌内容一致（避免 UI 同步遗漏导致"标 HS256 实签别的"）
    QJsonParseError parseError;
    const QJsonDocument headerDoc = QJsonDocument::fromJson(headerText.toUtf8(), &parseError);
    const QJsonObject headerObj = headerDoc.object();
    const QString alg = headerObj.value(QStringLiteral("alg")).toString();
    if (alg.isEmpty()) {
        r.error = QStringLiteral("Header 缺少 \"alg\" 字段（如 \"alg\":\"HS256\"）");
        return r;
    }
    r.alg = alg;

    // JWS 紧凑序列化：两段都是「JSON 文本 → base64url」，签名覆盖的是这两段 b64 文本
    const QString headerB64 = base64UrlEncode(headerText.toUtf8());
    const QString payloadB64 = base64UrlEncode(payloadText.toUtf8());
    const QByteArray signingInput = (headerB64 + QLatin1Char('.') + payloadB64).toUtf8();
    QByteArray signature;
    if (alg == QLatin1String("none")) {
        // alg=none 的规范形态就是空签名段（末尾保留 "."），不使用密钥
        signature.clear();
    } else {
        QCryptographicHash::Algorithm hmacAlg;
        if (!hmacAlgorithm(alg, &hmacAlg)) {
            r.error = QStringLiteral("不支持的算法 \"%1\"：生成仅支持 HS256 / HS384 / HS512 / none").arg(alg);
            return r;
        }
        if (secret.isEmpty()) {
            r.error = QStringLiteral("算法 %1 需要密钥").arg(alg);
            return r;
        }
        signature = hmacSign(hmacAlg, secret.toUtf8(), signingInput);
    }

    r.token = headerB64 + QLatin1Char('.') + payloadB64 + QLatin1Char('.')
              + base64UrlEncode(signature);
    r.ok = true;
    return r;
}

/// 把 Header JSON 里的 alg 改成指定值（编码页下拉框切换时用）。
/// 走 QJsonObject 重序列化，键序会被排序 —— 仅 Header 几个字段、语义无影响，
/// 且改完文本就是新的真源，后续编码仍按 compactJson() 保序处理。
inline QString setHeaderAlg(const QString &headerJson, const QString &alg)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(headerJson.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return headerJson;   // 原文不合法就原样返回，让用户在编辑器里看到自己的错误
    QJsonObject obj = doc.object();
    obj.insert(QStringLiteral("alg"), alg);
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented)).trimmed();
}

} // namespace jwtlogic

#pragma once

// ---------------------------------------------------------------------------
// HTTP 请求模型与纯逻辑工具。
//
// 这里刻意不 include 任何 QWidget：URL/查询串互转、生效请求头计算、cURL 互转、
// JSON 美化都能用一个临时断言程序直接 include 验证（AGENTS.md 规则 2）。
// 面板只负责把界面状态搬进 Spec、再把 Spec 交给 QNetworkAccessManager。
//
// 术语：
//   * Spec.url 永远是「不含查询串」的基地址（可以带 #fragment 之外的部分），
//     查询参数一律放在 Spec.params 里，由 joinUrl() 拼回去。这样参数表的
//     启用/禁用状态不会被 URL 里的原文覆盖。
//   * 「生效请求头」= 用户勾选的请求头 + 认证自动生成的请求头 + 按请求体类型
//     自动补的 Content-Type（用户自己写了就不补）。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

namespace httpapi {

/// 支持的方法。顺序即下拉框顺序，GET 在最前因为它是默认值。
inline const QStringList &methods()
{
    static const QStringList list = {
        QStringLiteral("GET"),    QStringLiteral("POST"), QStringLiteral("PUT"),
        QStringLiteral("PATCH"),  QStringLiteral("DELETE"), QStringLiteral("HEAD"),
        QStringLiteral("OPTIONS"),
    };
    return list;
}

/// 请求体类型。用枚举而不是自由文本，是为了让 Content-Type 与编辑器提示联动。
enum class BodyMode
{
    None,   ///< 无请求体（GET / HEAD 的常见形态）
    Json,   ///< raw JSON，自动补 application/json
    Text,   ///< raw 纯文本，自动补 text/plain
    Xml,    ///< raw XML，自动补 application/xml
    Form,   ///< x-www-form-urlencoded，自动补并规范化编码
};

/// 认证方式。只做最常用的四种，OAuth2 那种需要浏览器回调的不在工具范围内。
enum class AuthMode
{
    None,
    Bearer,   ///< Authorization: Bearer <token>
    Basic,    ///< Authorization: Basic base64(user:password)
    ApiKey,   ///< 自定义请求头或查询参数
};

/// API Key 放在请求头还是查询串里。
enum class KeyIn
{
    Header,
    Query,
};

/// 一对键值 + 是否启用。参数表和请求头表都复用它。
/// 保留「禁用但留在表里」的语义，和 Postman 一致：临时关掉一行不用删掉再重填。
struct KeyValue
{
    bool    enabled = true;
    QString key;
    QString value;
};

/// 认证配置。按 mode 取用其中一组字段，其余留空即可。
struct Auth
{
    AuthMode mode = AuthMode::None;
    QString  token;      ///< Bearer
    QString  user;       ///< Basic
    QString  password;   ///< Basic
    QString  keyName;    ///< ApiKey 的键名，如 X-Api-Key
    QString  keyValue;   ///< ApiKey 的值
    KeyIn    keyIn = KeyIn::Header;
};

/// 一次完整的请求。面板每次发送都从界面重新收集一份，不做隐式共享。
struct Spec
{
    QString           method = QStringLiteral("GET");
    QString           url;                   ///< 不含查询串的基地址
    QVector<KeyValue> params;                ///< 查询参数（含禁用项）
    QVector<KeyValue> headers;               ///< 请求头（含禁用项）
    BodyMode          bodyMode = BodyMode::None;
    QString           body;
    Auth              auth;
    QString           name;                  ///< 集合里保存的名字；草稿/历史为空

    bool isValid() const { return !url.trimmed().isEmpty(); }
};

// ---------------------------------------------------------------- 枚举 <-> 字符串

/// 落盘用短横线小写（"raw-json"），比枚举整数可读，改枚举顺序也不会串档。
inline QString bodyModeToken(BodyMode mode)
{
    switch (mode) {
    case BodyMode::Json: return QStringLiteral("json");
    case BodyMode::Text: return QStringLiteral("text");
    case BodyMode::Xml:  return QStringLiteral("xml");
    case BodyMode::Form: return QStringLiteral("form");
    case BodyMode::None: break;
    }
    return QStringLiteral("none");
}

inline BodyMode bodyModeFromToken(const QString &token)
{
    if (token == QLatin1String("json")) return BodyMode::Json;
    if (token == QLatin1String("text")) return BodyMode::Text;
    if (token == QLatin1String("xml"))  return BodyMode::Xml;
    if (token == QLatin1String("form")) return BodyMode::Form;
    return BodyMode::None;
}

inline QString authModeToken(AuthMode mode)
{
    switch (mode) {
    case AuthMode::Bearer: return QStringLiteral("bearer");
    case AuthMode::Basic:  return QStringLiteral("basic");
    case AuthMode::ApiKey: return QStringLiteral("apikey");
    case AuthMode::None:   break;
    }
    return QStringLiteral("none");
}

inline AuthMode authModeFromToken(const QString &token)
{
    if (token == QLatin1String("bearer")) return AuthMode::Bearer;
    if (token == QLatin1String("basic"))  return AuthMode::Basic;
    if (token == QLatin1String("apikey")) return AuthMode::ApiKey;
    return AuthMode::None;
}

inline QString keyInToken(KeyIn where)
{
    return where == KeyIn::Query ? QStringLiteral("query") : QStringLiteral("header");
}

inline KeyIn keyInFromToken(const QString &token)
{
    return token == QLatin1String("query") ? KeyIn::Query : KeyIn::Header;
}

/// 该请求体类型对应的 Content-Type。None 返回空（表示不要自动补）。
inline QString defaultContentType(BodyMode mode)
{
    switch (mode) {
    case BodyMode::Json: return QStringLiteral("application/json");
    case BodyMode::Text: return QStringLiteral("text/plain");
    case BodyMode::Xml:  return QStringLiteral("application/xml");
    case BodyMode::Form: return QStringLiteral("application/x-www-form-urlencoded");
    case BodyMode::None: break;
    }
    return QString();
}

// ---------------------------------------------------------------- URL 与查询串

/// splitUrl() 的结果。fragment 单独拿出来，拼接时再放回最末尾 ——
/// 否则 `http://a/b#x?y` 这种写法会把查询串拼进 fragment 里。
struct UrlParts
{
    QString           base;
    QString           fragment;
    QVector<KeyValue> params;
};

///
/// 把用户输入的地址拆成「基地址 + 查询参数」。
///
/// 不用 QUrl 拆是为了保留用户原样的基地址（QUrl::toString 会做编码归一化，
/// 用户看到的地址会莫名其妙变样）。只在解析/重建查询串时走 QUrlQuery，
/// 它能正确处理 `%20`、`+` 与重复键。
///
inline UrlParts splitUrl(const QString &text)
{
    UrlParts parts;
    QString rest = text.trimmed();

    const int hash = rest.indexOf(QLatin1Char('#'));
    if (hash >= 0) {
        parts.fragment = rest.mid(hash);
        rest.truncate(hash);
    }

    const int question = rest.indexOf(QLatin1Char('?'));
    if (question < 0) {
        parts.base = rest;
        return parts;
    }

    parts.base = rest.left(question);
    // QUrlQuery 接受不带 '?' 的查询串。FullyDecoded 让参数表里显示中文而不是 %E4%B8%AD。
    const QUrlQuery query(rest.mid(question + 1));
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        KeyValue kv;
        kv.enabled = true;
        kv.key = item.first;
        kv.value = item.second;
        parts.params.append(kv);
    }
    return parts;
}

///
/// 把基地址与启用的参数拼回完整地址。
/// 禁用行与「键值都为空」的占位行不会进 URL —— 参数表末尾常留一行空的。
///
inline QString joinUrl(const QString &base, const QVector<KeyValue> &params,
                       const QString &fragment = QString())
{
    QUrlQuery query;
    for (const KeyValue &kv : params) {
        if (!kv.enabled)
            continue;
        if (kv.key.isEmpty() && kv.value.isEmpty())
            continue;
        query.addQueryItem(kv.key, kv.value);
    }

    QString out = base.trimmed();
    if (!query.isEmpty())
        out += QLatin1Char('?') + query.toString(QUrl::FullyEncoded);
    if (!fragment.isEmpty())
        out += fragment;
    return out;
}

///
/// 用户输入的地址 → 可发送的 QUrl。
///
/// QUrl::fromUserInput 会自动把 `localhost:8080/api`、`127.0.0.1:8080` 这类
/// 「看着像 scheme 其实是主机」的写法补成 http://，这正是接口调试最常输的形态；
/// 仍然解析不出主机时再兜一次 http:// 前缀，避免用户被要求手写协议头。
///
inline QUrl toUrl(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QUrl();

    QUrl url = QUrl::fromUserInput(trimmed);
    if (!url.isValid() || url.host().isEmpty())
        url = QUrl(QStringLiteral("http://") + trimmed);
    return url;
}

/// 完整请求地址。
///
/// 参数有两个来源：Spec.params（面板的参数表）与 Spec.url 里附带的查询串
/// （从 cURL 导入时保留原样）。以参数表为准，表为空时才用地址里的 ——
/// 两处都拼会把同一批参数发两遍。
inline QString effectiveUrl(const Spec &spec)
{
    UrlParts parts = splitUrl(spec.url);
    QVector<KeyValue> params = spec.params;
    if (params.isEmpty())
        params = parts.params;

    if (spec.auth.mode == AuthMode::ApiKey && spec.auth.keyIn == KeyIn::Query
        && !spec.auth.keyName.trimmed().isEmpty()) {
        KeyValue kv;
        kv.key = spec.auth.keyName.trimmed();
        kv.value = spec.auth.keyValue;
        params.append(kv);
    }
    return joinUrl(parts.base, params, parts.fragment);
}

// ---------------------------------------------------------------- 请求头与请求体

/// 用户填写的请求头（只取启用且键非空的）。
inline QVector<QPair<QString, QString>> userHeaders(const Spec &spec)
{
    QVector<QPair<QString, QString>> out;
    for (const KeyValue &kv : spec.headers) {
        if (!kv.enabled || kv.key.trimmed().isEmpty())
            continue;
        out.append({kv.key.trimmed(), kv.value});
    }
    return out;
}

/// 是否已经手动写了某个请求头（大小写不敏感，HTTP 头名本来就不区分大小写）。
inline bool hasHeader(const QVector<QPair<QString, QString>> &headers, const QString &name)
{
    for (const auto &header : headers) {
        if (header.first.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

/// 实际会发出去的请求头：用户填的 + 认证 + 自动 Content-Type。
inline QVector<QPair<QString, QString>> effectiveHeaders(const Spec &spec)
{
    QVector<QPair<QString, QString>> out = userHeaders(spec);

    if (spec.bodyMode != BodyMode::None) {
        const QString contentType = defaultContentType(spec.bodyMode);
        if (!contentType.isEmpty() && !hasHeader(out, QStringLiteral("Content-Type")))
            out.append({QStringLiteral("Content-Type"), contentType});
    }

    switch (spec.auth.mode) {
    case AuthMode::Bearer:
        if (!spec.auth.token.isEmpty())
            out.append({QStringLiteral("Authorization"),
                        QStringLiteral("Bearer ") + spec.auth.token});
        break;
    case AuthMode::Basic: {
        // HTTP Basic 的规范写法是 user:password 的 UTF-8 字节再 base64。
        // 用 UTF-8 而不是本地编码：中文用户名/密码也能被服务端正确还原。
        const QByteArray raw = (spec.auth.user + QLatin1Char(':') + spec.auth.password).toUtf8();
        out.append({QStringLiteral("Authorization"),
                    QStringLiteral("Basic ") + QString::fromLatin1(raw.toBase64())});
        break;
    }
    case AuthMode::ApiKey:
        if (spec.auth.keyIn == KeyIn::Header && !spec.auth.keyName.trimmed().isEmpty())
            out.append({spec.auth.keyName.trimmed(), spec.auth.keyValue});
        break;
    case AuthMode::None:
        break;
    }
    return out;
}

///
/// 表单请求体的规范化。
///
/// 用户可能写成 `a=1&b=2`，也可能一行一个 `a=1`。直接原样发中文会变成非法字节，
/// 所以先按 QUrlQuery 解码再重新编码：已经编码过的 `%E4%B8%AD` 解→编一次仍是
/// `%E4%B8%AD`（幂等），没编码的中文则会被正确编码。
///
inline QString normalizeFormBody(const QString &body)
{
    QString text = body;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\n'), QLatin1Char('&'));

    const QUrlQuery input(text);
    QUrlQuery output;
    const auto items = input.queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        if (item.first.isEmpty() && item.second.isEmpty())
            continue;
        output.addQueryItem(item.first, item.second);
    }
    return output.toString(QUrl::FullyEncoded);
}

/// 请求体字节。空请求体返回空 QByteArray（QNAM 会因此不带 Content-Length）。
inline QByteArray bodyPayload(const Spec &spec)
{
    switch (spec.bodyMode) {
    case BodyMode::None:
        return QByteArray();
    case BodyMode::Form:
        return normalizeFormBody(spec.body).toUtf8();
    default:
        break;
    }
    // 统一按 UTF-8 发送：JSON/XML 的声明里通常也是 UTF-8，和 Content-Type 对得上。
    return spec.body.toUtf8();
}

// ---------------------------------------------------------------- JSON 展示

/// 美化 JSON。解析失败返回空串，调用方据此决定「保持原样」而不是把错误当成功。
inline QString prettyJson(const QByteArray &data)
{
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError)
        return QString();

    // toJson 末尾带换行，去掉后与编辑器的行数统计一致。
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
}

/// Content-Type 是否 JSON（含 application/*+json）。
inline bool isJsonContentType(const QString &contentType)
{
    const QString lower = contentType.toLower();
    return lower.contains(QLatin1String("application/json"))
        || lower.contains(QLatin1String("+json"));
}

/// 是否适合按文本展示。图片/音频/压缩包等二进制不往文本框里塞，避免整屏乱码。
inline bool isTextualContentType(const QString &contentType)
{
    if (contentType.isEmpty())
        return true;   // 没给 Content-Type 时按文本试读，服务端常有漏写的
    const QString lower = contentType.toLower();
    if (lower.startsWith(QLatin1String("text/")))
        return true;
    return lower.contains(QLatin1String("json")) || lower.contains(QLatin1String("xml"))
        || lower.contains(QLatin1String("javascript")) || lower.contains(QLatin1String("x-www-form-urlencoded"))
        || lower.contains(QLatin1String("html")) || lower.contains(QLatin1String("csv"))
        || lower.contains(QLatin1String("yaml"));
}

/// 人类可读的体积。响应大小要一眼能判断「是不是拉多了」。
inline QString formatBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

/// 耗时。1 秒以内按毫秒显示，超过就换秒，避免出现 "12345 ms" 这种难读的数。
inline QString formatDuration(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2);
}

// ---------------------------------------------------------------- cURL 互转

/// POSIX shell 单引号转义：把 ' 换成 '\''，其余原样。
inline QString shellQuote(const QString &text)
{
    QString escaped = text;
    escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

///
/// 生成可粘贴到终端的 cURL 命令。
///
/// 每行一个 -H 并用反斜杠续行，照着 Postman 的 Code 面板的排版：一行塞十几个头
/// 在终端里根本没法读。GET 不写 -X（curl 默认就是 GET），其余方法显式写出来。
///
inline QString toCurl(const Spec &spec, const QString &linePrefix = QString())
{
    QStringList parts;
    parts << QStringLiteral("curl");

    const QString method = spec.method.trimmed().toUpper();
    if (!method.isEmpty() && method != QLatin1String("GET"))
        parts << QStringLiteral("-X ") + method;

    parts << shellQuote(effectiveUrl(spec));

    const auto headers = effectiveHeaders(spec);
    for (const auto &header : headers)
        parts << QStringLiteral("-H ") + shellQuote(header.first + QStringLiteral(": ") + header.second);

    const QByteArray payload = bodyPayload(spec);
    if (!payload.isEmpty())
        parts << QStringLiteral("--data-raw ") + shellQuote(QString::fromUtf8(payload));

    QString out;
    for (int i = 0; i < parts.size(); ++i) {
        const bool last = (i == parts.size() - 1);
        out += linePrefix + parts.at(i);
        if (!last)
            out += QLatin1String(" \\\n");
    }
    return out;
}

///
/// 把一行 shell 命令切成 token：处理单引号、双引号、反斜杠续行。
/// 只覆盖 cURL 命令会出现的引号形态，不实现完整 shell 语法（不做变量展开、
/// 不做命令替换）—— 这是刻意的：展开 `$TOKEN` 会把用户的密钥写进历史记录。
///
inline QStringList tokenizeShell(const QString &text)
{
    QStringList tokens;
    QString current;
    bool hasToken = false;
    QChar quote;   // 空 = 不在引号里

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);

        if (ch == QLatin1Char('\\')) {
            if (i + 1 < text.size()) {
                const QChar next = text.at(i + 1);
                if (next == QLatin1Char('\n')) {   // 续行：反斜杠 + 换行整体丢掉
                    ++i;
                    continue;
                }
                if (next == QLatin1Char('\r')) {
                    ++i;
                    if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                        ++i;
                    continue;
                }
                // 双引号里反斜杠只转义少数几个字符；单引号里一律是字面量。
                if (quote == QLatin1Char('\'')) {
                    current += ch;
                    hasToken = true;
                    continue;
                }
                if (quote.isNull() || next == QLatin1Char('"') || next == QLatin1Char('\\')
                    || next == QLatin1Char('$') || next == QLatin1Char('`')) {
                    current += next;
                    hasToken = true;
                    ++i;
                    continue;
                }
                current += ch;
                hasToken = true;
                continue;
            }
            continue;   // 命令末尾的孤立反斜杠没有意义
        }

        if (!quote.isNull()) {
            if (ch == quote) {
                quote = QChar();
            } else {
                current += ch;
            }
            hasToken = true;
            continue;
        }

        if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) {
            quote = ch;
            hasToken = true;   // 空字符串 '' 也算一个参数
            continue;
        }

        if (ch.isSpace()) {
            if (hasToken) {
                tokens.append(current);
                current.clear();
                hasToken = false;
            }
            continue;
        }

        current += ch;
        hasToken = true;
    }

    if (hasToken)
        tokens.append(current);
    return tokens;
}

/// 从剪贴板里的 cURL 命令还原出请求。识别不了的开关直接跳过，不猜。
struct CurlParseResult
{
    bool   ok = false;
    QString error;
    Spec   spec;
};

inline CurlParseResult parseCurl(const QString &command)
{
    CurlParseResult result;
    QStringList tokens = tokenizeShell(command);
    if (tokens.isEmpty()) {
        result.error = QStringLiteral("命令为空");
        return result;
    }
    // 允许用户把整行 `curl 'http://…'` 一起复制进来。
    if (!tokens.first().isEmpty() && tokens.first().compare(QLatin1String("curl"), Qt::CaseInsensitive) == 0)
        tokens.removeFirst();

    QString method;
    QString url;
    QStringList headerLines;
    QString body;
    bool hasBody = false;
    QString basicUser;

    auto nextValue = [&tokens](int &index, QString *out) -> bool {
        if (index + 1 >= tokens.size())
            return false;
        *out = tokens.at(++index);
        return true;
    };

    for (int i = 0; i < tokens.size(); ++i) {
        const QString token = tokens.at(i);
        QString value;

        if (token == QLatin1String("-X") || token == QLatin1String("--request")) {
            if (nextValue(i, &value))
                method = value.trimmed().toUpper();
            continue;
        }
        if (token == QLatin1String("-H") || token == QLatin1String("--header")) {
            if (nextValue(i, &value))
                headerLines.append(value);
            continue;
        }
        if (token == QLatin1String("-d") || token == QLatin1String("--data")
            || token == QLatin1String("--data-raw") || token == QLatin1String("--data-binary")
            || token == QLatin1String("--data-ascii")) {
            if (nextValue(i, &value)) {
                if (hasBody)
                    body += QLatin1Char('&');
                body += value;
                hasBody = true;
            }
            continue;
        }
        if (token == QLatin1String("--data-urlencode")) {
            if (nextValue(i, &value)) {
                if (hasBody)
                    body += QLatin1Char('&');
                body += value;
                hasBody = true;
            }
            continue;
        }
        if (token == QLatin1String("-u") || token == QLatin1String("--user")) {
            if (nextValue(i, &value))
                basicUser = value;
            continue;
        }
        if (token == QLatin1String("-b") || token == QLatin1String("--cookie")) {
            if (nextValue(i, &value))
                headerLines.append(QStringLiteral("Cookie: ") + value);
            continue;
        }
        if (token == QLatin1String("-A") || token == QLatin1String("--user-agent")) {
            if (nextValue(i, &value))
                headerLines.append(QStringLiteral("User-Agent: ") + value);
            continue;
        }
        if (token == QLatin1String("--url")) {
            if (nextValue(i, &value))
                url = value;
            continue;
        }
        // 无参数的常用开关：压缩、静默、跟随跳转、忽略证书、只看响应头等。
        // 这些对「还原请求」没有影响，跳过即可。
        if (token == QLatin1String("--compressed") || token == QLatin1String("-s")
            || token == QLatin1String("--silent") || token == QLatin1String("-k")
            || token == QLatin1String("--insecure") || token == QLatin1String("-L")
            || token == QLatin1String("--location") || token == QLatin1String("-i")
            || token == QLatin1String("--include") || token == QLatin1String("-v")
            || token == QLatin1String("--verbose") || token == QLatin1String("-f")
            || token == QLatin1String("--fail")) {
            continue;
        }
        if (token.startsWith(QLatin1Char('-'))) {
            // 未知开关：如果它后面跟着的不是另一个开关，且当前还没有 URL，
            // 那大概率是这个开关的取值（如 --connect-timeout 5），一并吞掉。
            if (url.isEmpty() && i + 1 < tokens.size() && !tokens.at(i + 1).startsWith(QLatin1Char('-')))
                ++i;
            continue;
        }
        if (url.isEmpty())
            url = token;
    }

    if (url.trimmed().isEmpty()) {
        result.error = QStringLiteral("没找到请求地址");
        return result;
    }

    const UrlParts parts = splitUrl(url);
    Spec spec;
    spec.url = parts.base;
    spec.params = parts.params;
    spec.body = body;
    spec.bodyMode = hasBody ? BodyMode::Text : BodyMode::None;
    spec.method = method.isEmpty() ? (hasBody ? QStringLiteral("POST") : QStringLiteral("GET")) : method;

    for (const QString &line : headerLines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        KeyValue kv;
        kv.key = line.left(colon).trimmed();
        kv.value = line.mid(colon + 1).trimmed();

        // Authorization 与 Content-Type 被翻译成结构化字段，剩下的留在请求头表里。
        if (kv.key.compare(QLatin1String("Authorization"), Qt::CaseInsensitive) == 0
            && kv.value.startsWith(QLatin1String("Bearer "), Qt::CaseInsensitive)) {
            spec.auth.mode = AuthMode::Bearer;
            spec.auth.token = kv.value.mid(7).trimmed();
            continue;
        }
        if (kv.key.compare(QLatin1String("Content-Type"), Qt::CaseInsensitive) == 0) {
            const QString lower = kv.value.toLower();
            if (lower.contains(QLatin1String("json")))
                spec.bodyMode = BodyMode::Json;
            else if (lower.contains(QLatin1String("xml")))
                spec.bodyMode = BodyMode::Xml;
            else if (lower.contains(QLatin1String("x-www-form-urlencoded")))
                spec.bodyMode = BodyMode::Form;
            else if (lower.startsWith(QLatin1String("text/")))
                spec.bodyMode = BodyMode::Text;
            // Content-Type 留在表里，用户能看到 import 出来的原值。
        }
        spec.headers.append(kv);
    }

    if (!basicUser.isEmpty()) {
        const int colon = basicUser.indexOf(QLatin1Char(':'));
        spec.auth.mode = AuthMode::Basic;
        spec.auth.user = colon >= 0 ? basicUser.left(colon) : basicUser;
        spec.auth.password = colon >= 0 ? basicUser.mid(colon + 1) : QString();
    }

    // 没有 Content-Type 时按内容猜一下，否则 JSON 体会以 text/plain 发出去。
    if (hasBody && spec.bodyMode == BodyMode::Text) {
        const QString trimmed = body.trimmed();
        if (trimmed.startsWith(QLatin1Char('{')) || trimmed.startsWith(QLatin1Char('['))) {
            if (!prettyJson(trimmed.toUtf8()).isEmpty())
                spec.bodyMode = BodyMode::Json;
        }
    }

    result.ok = true;
    result.spec = spec;
    return result;
}

// ---------------------------------------------------------------- 落盘

inline QJsonObject keyValueToJson(const KeyValue &kv)
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), kv.enabled);
    object.insert(QStringLiteral("key"), kv.key);
    object.insert(QStringLiteral("value"), kv.value);
    return object;
}

inline KeyValue keyValueFromJson(const QJsonObject &object)
{
    KeyValue kv;
    kv.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    kv.key = object.value(QStringLiteral("key")).toString();
    kv.value = object.value(QStringLiteral("value")).toString();
    return kv;
}

inline QJsonArray keyValuesToJson(const QVector<KeyValue> &items)
{
    QJsonArray array;
    for (const KeyValue &kv : items)
        array.append(keyValueToJson(kv));
    return array;
}

inline QVector<KeyValue> keyValuesFromJson(const QJsonValue &value)
{
    QVector<KeyValue> items;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &entry : array)
        items.append(keyValueFromJson(entry.toObject()));
    return items;
}

inline QJsonObject specToJson(const Spec &spec)
{
    QJsonObject object;
    object.insert(QStringLiteral("method"), spec.method);
    object.insert(QStringLiteral("url"), spec.url);
    object.insert(QStringLiteral("params"), keyValuesToJson(spec.params));
    object.insert(QStringLiteral("headers"), keyValuesToJson(spec.headers));
    object.insert(QStringLiteral("bodyMode"), bodyModeToken(spec.bodyMode));
    object.insert(QStringLiteral("body"), spec.body);
    object.insert(QStringLiteral("name"), spec.name);

    QJsonObject auth;
    auth.insert(QStringLiteral("mode"), authModeToken(spec.auth.mode));
    auth.insert(QStringLiteral("token"), spec.auth.token);
    auth.insert(QStringLiteral("user"), spec.auth.user);
    auth.insert(QStringLiteral("password"), spec.auth.password);
    auth.insert(QStringLiteral("keyName"), spec.auth.keyName);
    auth.insert(QStringLiteral("keyValue"), spec.auth.keyValue);
    auth.insert(QStringLiteral("keyIn"), keyInToken(spec.auth.keyIn));
    object.insert(QStringLiteral("auth"), auth);
    return object;
}

inline Spec specFromJson(const QJsonObject &object)
{
    Spec spec;
    const QString method = object.value(QStringLiteral("method")).toString();
    if (!method.isEmpty())
        spec.method = method;
    spec.url = object.value(QStringLiteral("url")).toString();
    spec.params = keyValuesFromJson(object.value(QStringLiteral("params")));
    spec.headers = keyValuesFromJson(object.value(QStringLiteral("headers")));
    spec.bodyMode = bodyModeFromToken(object.value(QStringLiteral("bodyMode")).toString());
    spec.body = object.value(QStringLiteral("body")).toString();
    spec.name = object.value(QStringLiteral("name")).toString();

    const QJsonObject auth = object.value(QStringLiteral("auth")).toObject();
    spec.auth.mode = authModeFromToken(auth.value(QStringLiteral("mode")).toString());
    spec.auth.token = auth.value(QStringLiteral("token")).toString();
    spec.auth.user = auth.value(QStringLiteral("user")).toString();
    spec.auth.password = auth.value(QStringLiteral("password")).toString();
    spec.auth.keyName = auth.value(QStringLiteral("keyName")).toString();
    spec.auth.keyValue = auth.value(QStringLiteral("keyValue")).toString();
    spec.auth.keyIn = keyInFromToken(auth.value(QStringLiteral("keyIn")).toString());
    return spec;
}

} // namespace httpapi

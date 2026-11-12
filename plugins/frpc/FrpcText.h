#pragma once

// ---------------------------------------------------------------------------
// frpc 配置的纯逻辑：格式嗅探、轻量校验、服务端地址提取、日志分级、时长格式化。
//
// 为什么自己写扫描而不引入 TOML 解析库：
//   本工具要做的判断只有「必填项在不在、端口像不像端口、有没有 proxies」，
//   并不需要把配置还原成结构化对象（真正解析的是 frpc 自己）。为这点需求引入
//   一个 TOML 库，既增加编译依赖，又会把「解析失败」和「配置写错」两种错误混在
//   一起——用户的诉求是「告诉我哪一行少写了什么」，不是「第 7 行第 13 列语法错误」。
//   所以这里做的是**行级扫描**：够用、零依赖、可单独断言（见 AGENTS.md 规则 2）。
//
// 代价必须写清楚：这不是完整解析器。行内注释只按「引号外的 # / ;」切分，
// 多行字符串（"""…"""）与未加引号的复杂值（数组、内联表）只做存在性判断，
// 取值时可能拿到 `['a.com']` 这样的原文。凡是用于「提示用户」的结论都只到这里为止，
// 不做更精细的推断，宁可少报也不误报。
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>
#include <QVector>
#include <QRegularExpression>
#include <QSet>

namespace frpctext {

/// 配置文件格式。frp v0.52 起用 TOML/YAML/JSON，v0.51 及以前是 INI。
/// 本工具只处理 TOML 与 INI 两种：面板里是纯文本编辑，YAML/JSON 用户自己粘进来
/// 也能启动（frpc 认扩展名），但校验会按 TOML 走，可能给出不准确的提示。
enum class Format
{
    Toml,
    Ini
};

/// 格式的中文名，用于状态行与提示文案。
inline QString formatName(Format format)
{
    return format == Format::Ini ? QStringLiteral("INI") : QStringLiteral("TOML");
}

/// 落盘用的文件名。frpc 按扩展名判断格式，所以扩展名必须与内容匹配，
/// 否则 v0.52+ 会把 INI 当成 TOML 解析并报一堆读不懂的错。
inline QString configFileName(Format format)
{
    return format == Format::Ini ? QStringLiteral("frpc.ini") : QStringLiteral("frpc.toml");
}

/// 嗅探格式：只要出现 `[common]` 段就按 INI 处理。
///
/// 这是唯一可靠的信号：新版 TOML 没有 `[common]` 这个概念（服务端字段放在顶层），
/// 而 INI 配置**必须**有它。反过来判断（"没有 [common] 就是 TOML"）也成立，
/// 所以不必再猜别的。
inline Format sniffFormat(const QString &text)
{
    static const QRegularExpression commonRe(QStringLiteral(R"(^\s*\[\s*common\s*\]\s*$)"),
                                             QRegularExpression::MultilineOption);
    return text.contains(commonRe) ? Format::Ini : Format::Toml;
}

/// 新建配置时填的模板（TOML，对应 frp v0.52+）。
///
/// 刻意留成「填三个空就能跑」的样子：地址、端口、一个 ssh 转发示例。
/// 注释里写清 token 是可选的，避免用户以为不填就不能用。
inline QString defaultConfig()
{
    return QStringLiteral(
        "# frpc 配置（frp v0.52 及以后用 TOML；v0.51 及以前请改成 [common] 的 INI 写法）\n"
        "# 文档：https://gofrp.org/zh-cn/docs/\n"
        "\n"
        "serverAddr = \"127.0.0.1\"\n"
        "serverPort = 7000\n"
        "\n"
        "# 服务端开了 token 就把下面两行取消注释并填上；没开就保持注释状态\n"
        "# auth.method = \"token\"\n"
        "# auth.token = \"\"\n"
        "\n"
        "# 一条转发：把本机 22 端口映射到服务端的 6000 端口\n"
        "[[proxies]]\n"
        "name = \"ssh\"\n"
        "type = \"tcp\"\n"
        "localIP = \"127.0.0.1\"\n"
        "localPort = 22\n"
        "remotePort = 6000\n");
}

/// 校验结论里的一条问题。
struct Issue
{
    enum Level
    {
        Error, ///< 一定会启动失败，或启动后什么也不转发
        Warn   ///< 能启动，但很可能不是用户想要的结果
    };

    Level level = Error;
    int line = 0;      ///< 行号（1 起）；0 表示与具体行无关
    QString text;      ///< 直接展示给用户的中文原因

    /// 带行号的一句话，用于提示行与悬浮说明。
    QString describe() const
    {
        return line > 0 ? QStringLiteral("第 %1 行：%2").arg(line).arg(text) : text;
    }
};

/// 校验结果。
struct Validation
{
    QVector<Issue> issues;
    QVector<Issue> warnings;   ///< 与 issues 分开存，面板里「错误」优先展示
    bool hasError = false;

    /// 一行摘要：有错报第一条错，没错误但有警告报第一条警告，都没有就说没问题。
    QString summary() const
    {
        if (!issues.isEmpty())
            return issues.first().describe();
        if (!warnings.isEmpty())
            return warnings.first().describe();
        return QStringLiteral("配置看起来没问题");
    }
};

/// 配置里的一段（INI 的 `[段]`、TOML 的 `[table]` / `[[array]]`）。
/// 顶层键值对放在 name 为空的段里（TOML 才有这种形式）。
struct Section
{
    QString name;
    int line = 0;

    struct Pair
    {
        QString key;
        QString value;
        int line = 0;
    };

    QVector<Pair> pairs;
};

/// 把「引号外的 # 或 ; 之后」当注释切掉。
///
/// 注意两侧都用不了「找第一个 #」这种偷懒写法：`auth.token = "a#b"` 是合法配置，
/// 切在 # 上会把值截断，随后的「token 为空」判断就会误报。
inline QString stripInlineComment(const QString &line)
{
    bool inSingle = false;
    bool inDouble = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"') && !inSingle) {
            inDouble = !inDouble;
        } else if (ch == QLatin1Char('\'') && !inDouble) {
            inSingle = !inSingle;
        } else if ((ch == QLatin1Char('#') || ch == QLatin1Char(';')) && !inSingle && !inDouble) {
            return line.left(i);
        }
    }
    return line;
}

/// 去掉值两侧的引号（成对才去，避免把 `"abc` 这种残缺值也"修好"）。
inline QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2) {
        const QChar first = value.at(0);
        if ((first == QLatin1Char('"') || first == QLatin1Char('\''))
            && value.at(value.size() - 1) == first) {
            return value.mid(1, value.size() - 2);
        }
    }
    return value;
}

/// 行级扫描出所有段与键值对。
///
/// 非法行（没有 `=`、段名没闭合）直接跳过：把它们报成错误会把「frp 支持但本工具
/// 没实现」的写法误判成用户写错了。真正的语法错误由 frpc 自己报，日志里看得见。
inline QVector<Section> parseSections(const QString &text, Format format)
{
    Q_UNUSED(format);   // 两种格式的分段规则恰好一致（[x] / [[x]]），留参数是为了意图清晰

    QVector<Section> sections;
    Section current;
    const QStringList lines = text.split(QLatin1Char('\n'));

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = stripInlineComment(lines.at(i)).trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            if (!current.name.isEmpty() || !current.pairs.isEmpty())
                sections.append(current);
            current = Section();
            current.line = i + 1;
            if (line.startsWith(QStringLiteral("[[")))
                current.name = line.mid(2, line.size() - 4).trimmed();
            else
                current.name = line.mid(1, line.size() - 2).trimmed();
            continue;
        }

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;

        Section::Pair pair;
        pair.key = line.left(eq).trimmed();
        pair.value = unquote(line.mid(eq + 1).trimmed());
        pair.line = i + 1;
        current.pairs.append(pair);
    }

    if (!current.name.isEmpty() || !current.pairs.isEmpty())
        sections.append(current);
    return sections;
}

/// 在段里按键名找值。names 给出所有等价写法（TOML 的 camelCase 与 snake_case
/// frp 都接受，用户从旧文档抄过来的常常是 snake_case）。
inline const Section::Pair *findPair(const Section &section, const QStringList &names)
{
    for (const Section::Pair &pair : section.pairs) {
        for (const QString &name : names) {
            if (pair.key.compare(name, Qt::CaseInsensitive) == 0)
                return &pair;
        }
    }
    return nullptr;
}

/// 在所有段里找键（用于 `log.to` 这种可能出现在顶层也可能在 `[log]` 段的配置）。
inline const Section::Pair *findPairAnywhere(const QVector<Section> &sections,
                                             const QStringList &names)
{
    for (const Section &section : sections) {
        if (const Section::Pair *pair = findPair(section, names))
            return pair;
    }
    return nullptr;
}

/// 端口字符串是否像样。只认 1–65535 的纯数字：
/// frpc 允许 `remotePort` 写字符串，但写错时启动才报错，前置判断能省一次失败。
inline bool looksLikePort(const QString &value)
{
    bool ok = false;
    const int port = value.trimmed().toInt(&ok);
    return ok && port > 0 && port <= 65535;
}

/// 校验配置。
///
/// @param format 通常来自 sniffFormat()，也允许调用方强制指定（面板里没有格式切换，
///               但测试需要覆盖两种分支）。
///
/// 检查项全部是「frpc 启动时会失败」或「启动了但白跑」两类，不做风格检查。
inline Validation validate(const QString &text, Format format)
{
    Validation result;

    if (text.trimmed().isEmpty()) {
        result.hasError = true;
        result.issues.append({Issue::Error, 0, QStringLiteral("配置是空的，先填服务端地址")});
        return result;
    }

    const QVector<Section> sections = parseSections(text, format);

    // 顶层段（TOML）：parseSections 只在真的有顶层键时才产生 name 为空的段。
    const Section *top = nullptr;
    if (!sections.isEmpty() && sections.first().name.isEmpty())
        top = &sections.first();

    const auto addError = [&result](int line, const QString &text) {
        result.issues.append({Issue::Error, line, text});
        result.hasError = true;
    };
    const auto addWarn = [&result](int line, const QString &text) {
        result.warnings.append({Issue::Warn, line, text});
    };

    // 服务端字段：TOML 在顶层，INI 在 [common] 段。
    const Section *common = nullptr;
    if (format == Format::Ini) {
        for (const Section &section : sections) {
            if (section.name.compare(QStringLiteral("common"), Qt::CaseInsensitive) == 0) {
                common = &section;
                break;
            }
        }
        if (!common)
            addError(0, QStringLiteral("INI 配置必须有 [common] 段（服务端地址写在这里）"));
    }
    const Section *server = format == Format::Ini ? common : top;

    const Section::Pair *addr = nullptr;
    const Section::Pair *port = nullptr;
    if (server) {
        addr = findPair(*server, {QStringLiteral("serverAddr"), QStringLiteral("server_addr")});
        port = findPair(*server, {QStringLiteral("serverPort"), QStringLiteral("server_port")});
    }

    if (!addr || addr->value.trimmed().isEmpty()) {
        addError(server ? server->line : 0,
                 format == Format::Ini
                     ? QStringLiteral("缺少 server_addr（服务端地址）")
                     : QStringLiteral("缺少 serverAddr（服务端地址），TOML 写法下它在文件顶层"));
    }
    if (port && !looksLikePort(port->value))
        addError(port->line, QStringLiteral("serverPort 必须是 1–65535 之间的数字，现在是 \"%1\"")
                                 .arg(port->value));
    if (!port && addr)
        addWarn(addr->line, QStringLiteral("没写 serverPort，将使用默认端口 7000（服务端不是 7000 就得补上）"));

    // 日志去向：frpc 若把日志写文件，面板这份日志区就永远是空的。
    // 这是「功能看起来坏了」的典型来源，必须提前说清楚，而不是等用户来问。
    //
    // `log.to` 有两种写法：顶层点号键（`log.to = "file"`），或独立表 `[log]` 里的
    // `to = "file"`。只认前者会漏掉更常见的那种，所以两种都要查。
    const Section::Pair *logTo =
        findPairAnywhere(sections, {QStringLiteral("log.to"), QStringLiteral("logTo")});
    if (!logTo) {
        for (const Section &section : sections) {
            if (section.name.compare(QStringLiteral("log"), Qt::CaseInsensitive) != 0)
                continue;
            if (const Section::Pair *to = findPair(section, {QStringLiteral("to")})) {
                logTo = to;
                break;
            }
        }
    }
    if (logTo) {
        if (logTo->value.trimmed().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
            addWarn(logTo->line,
                    QStringLiteral("log.to = \"file\"：frpc 会把日志写进配置文件指定的文件，"
                                   "这里看不到输出（想在本工具里看日志就删掉这一行）"));
        }
    }

    // 逐段检查转发规则。TOML 是 [[proxies]] / [[visitors]]，INI 是任意段名。
    int proxyCount = 0;
    QSet<QString> seenNames;
    for (const Section &section : sections) {
        const bool isProxy = format == Format::Ini
            ? (!section.name.isEmpty()
               && section.name.compare(QStringLiteral("common"), Qt::CaseInsensitive) != 0)
            : section.name.compare(QStringLiteral("proxies"), Qt::CaseInsensitive) == 0;
        if (!isProxy)
            continue;
        ++proxyCount;

        const Section::Pair *name = findPair(section, {QStringLiteral("name")});
        const Section::Pair *type = findPair(section, {QStringLiteral("type")});
        const Section::Pair *localPort =
            findPair(section, {QStringLiteral("localPort"), QStringLiteral("local_port")});
        const Section::Pair *remotePort =
            findPair(section, {QStringLiteral("remotePort"), QStringLiteral("remote_port")});
        const Section::Pair *domains = findPair(section, {QStringLiteral("customDomains"),
                                                         QStringLiteral("custom_domains"),
                                                         QStringLiteral("subdomain")});

        if (!name && format == Format::Toml)
            addError(section.line, QStringLiteral("这一条 [[proxies]] 没写 name，frpc 会拒绝启动"));
        if (name && !name->value.trimmed().isEmpty()) {
            const QString key = name->value.trimmed();
            if (seenNames.contains(key))
                addError(name->line, QStringLiteral("转发名 \"%1\" 重复了，同一个客户端里名字必须唯一").arg(key));
            seenNames.insert(key);
        }

        if (!type) {
            addError(section.line, QStringLiteral("缺少 type（tcp / udp / http / https / stcp / xtcp / sudp）"));
            continue;
        }

        const QString typeValue = type->value.trimmed().toLower();
        static const QStringList knownTypes = {
            QStringLiteral("tcp"), QStringLiteral("udp"), QStringLiteral("http"),
            QStringLiteral("https"), QStringLiteral("stcp"), QStringLiteral("xtcp"),
            QStringLiteral("sudp"), QStringLiteral("tcpmux")};
        if (!knownTypes.contains(typeValue))
            addWarn(type->line, QStringLiteral("不认识的 type \"%1\"，确认一下拼写").arg(type->value));

        if (!localPort) {
            addError(section.line, QStringLiteral("缺少 localPort（要转发的本机端口）"));
        } else if (!looksLikePort(localPort->value)) {
            addError(localPort->line,
                     QStringLiteral("localPort 必须是 1–65535 之间的数字，现在是 \"%1\"")
                         .arg(localPort->value));
        }

        const bool httpLike = typeValue == QLatin1String("http")
            || typeValue == QLatin1String("https") || typeValue == QLatin1String("tcpmux");
        if (httpLike) {
            if (!domains)
                addWarn(type->line,
                        QStringLiteral("http / https 类型一般还要 customDomains（绑定的域名），"
                                       "只有 subdomain 或服务端配置了默认域名时才不用写"));
        } else if (typeValue != QLatin1String("stcp") && typeValue != QLatin1String("xtcp")) {
            // stcp / xtcp 走密钥点对点，服务端不分配端口；其余类型都要 remotePort 才能被访问。
            if (!remotePort)
                addError(section.line,
                         QStringLiteral("缺少 remotePort（服务端对外暴露的端口），否则这条转发没人能连上"));
            else if (!looksLikePort(remotePort->value))
                addError(remotePort->line,
                         QStringLiteral("remotePort 必须是 1–65535 之间的数字，现在是 \"%1\"")
                             .arg(remotePort->value));
        }
    }

    if (proxyCount == 0)
        addWarn(0, QStringLiteral("没有任何转发规则（proxies），启动后只会连上服务端，不转发任何东西"));

    return result;
}

/// 从配置里取服务端地址与端口，供「探活」按钮做一次 TCP 连接测试。
struct Endpoint
{
    QString host;
    int port = 7000;   ///< frp 默认端口
    int line = 0;      ///< 写 serverAddr / serverPort 的那一行，报错时能指回去
    bool valid = false;
    QString error;     ///< valid == false 时的可读原因
};

inline Endpoint endpoint(const QString &text, Format format)
{
    Endpoint result;

    const QVector<Section> sections = parseSections(text, format);
    const Section *server = nullptr;
    if (format == Format::Ini) {
        for (const Section &section : sections) {
            if (section.name.compare(QStringLiteral("common"), Qt::CaseInsensitive) == 0) {
                server = &section;
                break;
            }
        }
    } else if (!sections.isEmpty() && sections.first().name.isEmpty()) {
        server = &sections.first();
    }

    if (!server) {
        result.error = QStringLiteral("没找到服务端地址（%1）")
                           .arg(format == Format::Ini ? QStringLiteral("[common] 段")
                                                      : QStringLiteral("顶层 serverAddr"));
        return result;
    }

    const Section::Pair *addr = findPair(*server, {QStringLiteral("serverAddr"),
                                                   QStringLiteral("server_addr")});
    const Section::Pair *port = findPair(*server, {QStringLiteral("serverPort"),
                                                   QStringLiteral("server_port")});
    if (!addr || addr->value.trimmed().isEmpty()) {
        result.error = QStringLiteral("还没填服务端地址");
        return result;
    }

    result.host = addr->value.trimmed();
    result.line = addr->line;
    if (port && looksLikePort(port->value))
        result.port = port->value.trimmed().toInt();
    result.valid = true;
    return result;
}

/// 日志行的语义分级，只用于上色。
///
/// 判定顺序必须是「错误 → 警告 → 成功」：frpc 的 `login to server failed: ...`
/// 同时含 "failed" 与 "server"，若先判成功就会把失败涂成绿色。
enum class LogKind
{
    Info,
    Success,
    Warn,
    Error
};

inline LogKind classifyLogLine(const QString &raw)
{
    const QString line = raw.trimmed();
    if (line.isEmpty())
        return LogKind::Info;

    const QString lower = line.toLower();

    // frpc 的日志形如 `2024-05-01 10:00:00.123 [E] [client/service.go:1] ...`，
    // 级别字母比正文关键词更权威，先认它。
    if (lower.contains(QStringLiteral("[e]")) || lower.contains(QStringLiteral("[error]")))
        return LogKind::Error;
    if (lower.contains(QStringLiteral("[w]")) || lower.contains(QStringLiteral("[warn]")))
        return LogKind::Warn;

    if (lower.contains(QStringLiteral("error")) || lower.contains(QStringLiteral("failed"))
        || lower.contains(QStringLiteral("failure")) || lower.contains(QStringLiteral("panic"))) {
        return LogKind::Error;
    }
    if (lower.contains(QStringLiteral("warn")))
        return LogKind::Warn;
    if (lower.contains(QStringLiteral("success")) || lower.contains(QStringLiteral("started")))
        return LogKind::Success;

    return LogKind::Info;
}

/// 把秒数格式化成「1 小时 02 分 03 秒」这种给人看的时长。
/// 不足一分钟只显示秒；超过一天加上天数（frpc 挂几天不稀奇）。
inline QString formatUptime(qint64 seconds)
{
    if (seconds < 0)
        seconds = 0;

    const qint64 days = seconds / 86400;
    const qint64 hours = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;

    QString text;
    if (days > 0)
        text += QStringLiteral("%1 天 ").arg(days);
    if (days > 0 || hours > 0)
        text += QStringLiteral("%1 小时 ").arg(hours);
    if (days > 0 || hours > 0 || minutes > 0)
        text += QStringLiteral("%1 分 ").arg(minutes, 2, 10, QLatin1Char('0'));
    text += QStringLiteral("%1 秒").arg(secs, 2, 10, QLatin1Char('0'));
    return text;
}

} // namespace frpctext

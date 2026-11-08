#pragma once

// ---------------------------------------------------------------------------
// WebSocket 调试插件 —— 协议层（RFC 6455 的客户端与服务端两半）。
//
// 职责：地址归一化、握手报文组装与校验、帧编解码、关闭码文案。
// 这里是**纯逻辑**：不碰 socket、不碰 UI，只有 Qt 的值类型，
// 因此可以写个临时程序直接 include 做断言（AGENTS.md 规则 2）。
//
// 为什么不直接用 QWebSocket / QWebSocketServer：
//   本项目锁定的 Qt 6.8.3 安装里**没有 Qt WebSockets 模块**
//   （lib/cmake 下无 Qt6WebSockets，bin 下也没有 Qt6WebSockets.dll），
//   为一个调试工具让三平台用户回去装模块，代价比实现这 500 行更大。
//   需要的只有「握手 + 帧编解码」，用 QTcpSocket / QSslSocket 自己走更省依赖。
//
// 客户端与服务端的差异只有三处，全部收在本文件里：
//   1. 握手：客户端组装 GET 请求并校验 101 响应；服务端解析请求并回 101 / 4xx；
//   2. 掩码：客户端发出的帧必须带掩码，服务端发出的帧必须不带（RFC 6455 §5.1）；
//      收帧方向反过来 —— FrameReader 用 expectMasked 区分两种角色；
//   3. 关闭码文案：两端共用。
//
// 刻意没做的：
//   * 扩展协商（permessage-deflate 等）：一旦协商了扩展，帧的 RSV 位与载荷语义都会变；
//     本工具两端一律不申请扩展，收到 RSV 非 0 的帧直接判协议错误。
//   * wss 服务端：不做 TLS 监听（要证书与私钥输入，超出"简易"范围）；
//     调试 wss 客户端请连公网测试服务，见 README「已知边界」。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QCryptographicHash>
#include <QMap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace wsproto {

/// 帧类型。0x0 是分片消息的续帧，≥0x8 的都是控制帧（不能分片、载荷 ≤125）。
enum Opcode {
    OpContinuation = 0x0,
    OpText         = 0x1,
    OpBinary       = 0x2,
    OpClose        = 0x8,
    OpPing         = 0x9,
    OpPong         = 0xA,
};

/// 单条消息（含分片拼装后）的硬上限，防止对端用超大帧把内存打爆。
constexpr quint64 kMaxMessageBytes = 8ull * 1024 * 1024;

/// 握手用的固定 GUID（RFC 6455 §1.3），拼在 Sec-WebSocket-Key 后面算 SHA-1。
constexpr const char *kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// 解析后的连接地址。
struct Url
{
    bool    valid  = false;
    bool    secure = false;   ///< true = wss（TLS），false = ws
    QString host;             ///< 不带方括号的 IPv6 也直接放这里，拼 Host 头时再加
    quint16 port   = 0;
    QString target;           ///< 请求行里的路径 + 查询串，至少是 "/"
    QString error;            ///< valid == false 时给用户看的原因（中文）
};

///
/// 把用户输入的地址解析成连接参数。
///
/// 只接受 ws:// 与 wss://：缺 scheme 时不做"猜协议"（`127.0.0.1:8080`
/// 会被 QUrl 解析成 scheme="127.0.0.1"，猜出来的结果更误导人）。
/// 端口留空按 80 / 443 补；带 `#片段` 直接拒绝 —— RFC 6455 的 request-target
/// 里不允许 fragment，静默丢掉会让用户以为连的是另一个地址。
///
Url parseUrl(const QString &text);

/// 生成握手用的 Sec-WebSocket-Key：16 个随机字节的 base64（RFC 要求）。
QByteArray makeKey();

///
/// 组装握手请求（含结尾空行，可直接写进 socket）。
///
/// extraHeaders 必须是已经规整好的 `Name: Value` 行（见 sanitizeHeaders）。
/// 这里不做去重：调用方负责过滤掉握手自身占用的头，否则会出现两个
/// Sec-WebSocket-Key，服务器一般直接回 400 且原因难查。
///
QString buildHandshake(const Url &url, const QByteArray &key,
                       const QStringList &protocols, const QStringList &extraHeaders);

/// 校验服务器回的 Sec-WebSocket-Accept（base64(SHA1(key + GUID))）。
bool checkAccept(const QByteArray &key, const QString &acceptHeader);

///
/// 规整用户填的「自定义请求头」文本框。
///
/// 做三件事：按行切、跳过空行与 `#` 注释、把 `Name:Value` 补成 `Name: Value`；
/// 顺手丢掉握手自身的保留头（Host / Upgrade / Connection / Sec-WebSocket-*），
/// 因为这些值由本工具按 RFC 计算，用户手填只会造成重复。行内不允许出现 CR/LF
/// （避免头注入），不合法或没有冒号的行整行丢弃 —— 宁可少发一个头，
/// 也不要把半截内容按 HTTP 头发出去。
///
QStringList sanitizeHeaders(const QString &text);

/// 子协议输入框：逗号 / 空格 / 分号分隔都可以，去空、去重、保序。
QStringList parseProtocols(const QString &text);

/// 一个完整的 WebSocket 帧。
struct Frame
{
    int        opcode = OpContinuation;
    bool       fin    = true;
    QByteArray payload;
};

///
/// 帧解析器：把 TCP 字节流切成帧，不完整的尾部留在内部缓冲等下一次 feed。
///
/// 一次 next() 只吐一帧；帧还没收全就返回 false（不是错误）。
/// 出错（协议违规、超上限）时 failed() 为真、error() 给中文原因，
/// 之后所有 next() 都返回 false —— 上层看到 failed() 就应该断开连接。
///
/// expectMasked 区分收帧方的角色：**服务端**收客户端帧，必须带掩码（expectMasked
/// = true，解析时顺带解掩码）；**客户端**收服务端帧，必须不带掩码（默认 false）。
/// 方向判反了会把每组载荷都解成乱码，所以构造参数是显式的、没有默认值以外的猜测。
///
class FrameReader
{
public:
    explicit FrameReader(bool expectMasked = false)
        : m_expectMasked(expectMasked)
    {
    }

    void feed(const QByteArray &data) { m_buffer += data; }

    /// 取下一个完整帧。返回 false 表示"还没收全"或"已出错"。
    bool next(Frame *frame);

    bool    failed() const { return !m_error.isEmpty(); }
    QString error() const { return m_error; }

    void reset()
    {
        m_buffer.clear();
        m_error.clear();
    }

private:
    QByteArray m_buffer;
    QString    m_error;
    bool       m_expectMasked = false;
};

///
/// 把一条消息编成帧。
///
/// mask = true 是**客户端**行为（RFC 6455 §5.3 要求，服务端收到不带掩码的帧应判错）；
/// mask = false 是**服务端**行为（服务端发掩码帧同样违规）。
/// 载荷 <126 / <65536 / 更大 分别用 7 位、16 位、64 位长度字段；
/// 掩码是 4 个随机字节，按 4 字节循环异或载荷。
///
QByteArray encodeFrame(int opcode, const QByteArray &payload, bool mask = true);

/// 关闭帧的载荷：2 字节大端状态码 + 可选 UTF-8 原因（总长 ≤125，超出截断）。
QByteArray closePayload(int code, const QString &reason);

/// base64(SHA1(clientKey + GUID))：握手应答里的 Sec-WebSocket-Accept。
/// 客户端用它校验服务端，服务端用它回客户端，算法只有这一份。
QByteArray acceptFor(const QByteArray &clientKey);

/// HTTP 响应头解析结果。
struct HttpHead
{
    bool    complete = false;   ///< 是否已收到完整的 \r\n\r\n
    int     status   = 0;       ///< 状态码；complete 且解析失败时保持 0
    int     length   = 0;       ///< 头部字节数，用来切出后面紧跟的帧数据
    QString error;              ///< 非空表示报文不合法（不是"还没收全"）
    QMap<QString, QString> headers;   ///< 键统一小写，方便查 Sec-WebSocket-Accept
};

/// 解析握手响应。complete == false 且 error 为空表示数据还不够，继续 feed。
HttpHead parseHttpHead(const QByteArray &data);

///
/// 服务端侧：解析客户端发来的握手请求。
///
/// 与 parseHttpHead 分开写而不是加个"请求/响应"开关：两者除了首行格式以外，
/// 错误文案与上限判定也完全不同（请求要看方法/路径，响应要看状态码），
/// 合成一个函数只会让两边的分支互相污染。
///
struct HttpRequest
{
    bool    complete = false;    ///< 是否已收到完整的 \r\n\r\n
    QString method;              ///< GET / POST …
    QString target;              ///< 请求行里的路径 + 查询串
    QString httpVersion;         ///< 例如 "1.1"
    int     length   = 0;        ///< 头部字节数，用来切出后面紧跟的帧数据
    QString error;               ///< 非空表示报文不合法（不是"还没收全"）
    QMap<QString, QString> headers;   ///< 键统一小写
};

HttpRequest parseHttpRequest(const QByteArray &data);

///
/// 校验握手请求是否满足 RFC 6455 §4.2.1 的必要条件。
///
/// 返回空字符串表示可以接受握手；否则返回**给用户看**的中文原因，
/// 并把建议的 HTTP 状态码写进 *status（400 报文错误 / 405 方法不对 / 426 版本不符）。
/// 只校验"必须项"：路径、Origin、Cookie 一律不设限 —— 这是本地调试服务端，
/// 由用户自己决定要把哪个客户端连进来。
///
QString validateHandshake(const HttpRequest &req, int *status);

///
/// 选择子协议。
///
/// supported 为空 = 用户没填，直接回显客户端提出的第一个（这样任何客户端都能连上，
/// 是"简易服务端"最省事的默认）；非空 = 只接受交集里的第一个，
/// 交集为空且客户端确实提了协议时把原因写进 *error（应当拒绝握手）。
///
QString selectProtocol(const HttpRequest &req, const QStringList &supported, QString *error);

/// 101 响应报文（含结尾空行）。protocol 为空则不回 Sec-WebSocket-Protocol。
QByteArray buildAcceptResponse(const QByteArray &clientKey, const QString &protocol);

///
/// 握手被拒时的明文 HTTP 响应（Connection: close，回完就可以断开）。
///
/// 426 会额外带 `Sec-WebSocket-Version: 13`，RFC 要求版本不符时必须告知支持的版本，
/// 否则浏览器只会显示一句无用的"连接失败"。
///
QByteArray buildRejectResponse(int status, const QString &message);

/// 关闭码转中文说明（1000–1015 与 3000+ 的自定义段）。
QString closeCodeText(int code);

// ===========================================================================
// 以下是实现。全部 inline：本文件被设计成可单独 include 的纯逻辑单元
// （详见 AGENTS.md 规则 2），不额外生成一个需要链接的目标文件。
// ===========================================================================

inline Url parseUrl(const QString &text)
{
    Url result;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        result.error = QStringLiteral("地址为空：请填 ws:// 或 wss:// 开头的地址");
        return result;
    }

    // StrictMode：把含非法字符的地址当成错误，而不是偷偷百分比编码成另一个地址
    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid()) {
        result.error = QStringLiteral("地址不合法：%1").arg(url.errorString());
        return result;
    }
    if (url.scheme().isEmpty()) {
        result.error = QStringLiteral("地址缺少协议头：请写成 ws://host:port 或 wss://host");
        return result;
    }

    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("ws") && scheme != QLatin1String("wss")) {
        result.error = QStringLiteral("只支持 ws:// 与 wss://，当前是 %1://").arg(scheme);
        return result;
    }
    if (!url.fragment().isEmpty()) {
        // WebSocket 的 request-target 不允许带 fragment，静默丢弃会让用户连到
        // 一个"看起来不一样"的地址，索性直接拒绝。
        result.error = QStringLiteral("地址里不能带 #片段，请去掉 # 及其后的内容");
        return result;
    }
    if (url.host().isEmpty()) {
        result.error = QStringLiteral("地址里没有主机名");
        return result;
    }

    result.secure = (scheme == QLatin1String("wss"));
    result.host   = url.host();
    // port(默认值)：用户没写端口时直接给出协议默认端口，省掉后面每处判空
    result.port   = static_cast<quint16>(url.port(result.secure ? 443 : 80));

    // FullyEncoded：路径里的中文 / 空格必须按百分号编码发出，不能被 QUrl 解码
    const QString path = url.path(QUrl::FullyEncoded);
    const QString query = url.query(QUrl::FullyEncoded);
    result.target = path.isEmpty() ? QStringLiteral("/") : path;
    if (!query.isEmpty())
        result.target += QLatin1Char('?') + query;

    result.valid = true;
    return result;
}

inline QByteArray makeKey()
{
    // 16 字节随机 → base64（24 字符）。QRandomGenerator::system() 在 MSVC 下走
    // BCryptGenRandom，够用；这里不需要密码学强度之外的东西。
    QByteArray raw(16, Qt::Uninitialized);
    for (int i = 0; i < raw.size(); ++i)
        raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return raw.toBase64();
}

inline QString buildHandshake(const Url &url, const QByteArray &key,
                              const QStringList &protocols, const QStringList &extraHeaders)
{
    QString hostHeader = url.host;
    if (hostHeader.contains(QLatin1Char(':')))
        hostHeader = QLatin1Char('[') + hostHeader + QLatin1Char(']');   // IPv6 字面量
    const quint16 defaultPort = url.secure ? 443 : 80;
    if (url.port != defaultPort)
        hostHeader += QLatin1Char(':') + QString::number(url.port);

    QStringList lines;
    lines << QStringLiteral("GET %1 HTTP/1.1").arg(url.target);
    lines << QStringLiteral("Host: %1").arg(hostHeader);
    lines << QStringLiteral("Upgrade: websocket");
    lines << QStringLiteral("Connection: Upgrade");
    lines << QStringLiteral("Sec-WebSocket-Key: %1").arg(QString::fromLatin1(key));
    lines << QStringLiteral("Sec-WebSocket-Version: 13");
    if (!protocols.isEmpty())
        lines << QStringLiteral("Sec-WebSocket-Protocol: %1").arg(protocols.join(QStringLiteral(", ")));
    lines += extraHeaders;

    // 末尾两个空串 + join("\r\n") == 以空行结束的头部，RFC 7230 要求
    lines << QString() << QString();
    return lines.join(QStringLiteral("\r\n"));
}

inline QByteArray acceptFor(const QByteArray &clientKey)
{
    return QCryptographicHash::hash(clientKey + QByteArray(kAcceptGuid),
                                    QCryptographicHash::Sha1).toBase64();
}

inline bool checkAccept(const QByteArray &key, const QString &acceptHeader)
{
    return acceptFor(key) == acceptHeader.trimmed().toLatin1();
}

inline QStringList sanitizeHeaders(const QString &text)
{
    static const QStringList reserved = {
        QStringLiteral("host"), QStringLiteral("upgrade"), QStringLiteral("connection"),
        QStringLiteral("sec-websocket-key"), QStringLiteral("sec-websocket-version"),
        QStringLiteral("sec-websocket-protocol"), QStringLiteral("sec-websocket-extensions"),
    };

    QStringList out;
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    for (QString line : rawLines) {
        line.remove(QLatin1Char('\r'));
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;   // 没有冒号或名称为空：不是合法头，整行丢弃

        const QString name = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (name.isEmpty() || value.isEmpty())
            continue;
        if (name.contains(QLatin1Char(' ')) || name.contains(QLatin1Char('\t')))
            continue;   // 头名称里不允许空白，多半是用户写错了
        if (reserved.contains(name.toLower()))
            continue;

        out << name + QStringLiteral(": ") + value;
    }
    return out;
}

inline QStringList parseProtocols(const QString &text)
{
    QStringList out;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;，；\\s]+")),
                                         Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty() && !out.contains(trimmed))
            out << trimmed;
    }
    return out;
}

inline QByteArray encodeFrame(int opcode, const QByteArray &payload, bool mask)
{
    QByteArray frame;
    frame.reserve(payload.size() + (mask ? 14 : 10));
    frame.append(static_cast<char>(0x80 | (opcode & 0x0F)));   // FIN=1 + opcode

    const qsizetype len = payload.size();
    // MASK 位在长度字段的最高位：服务端（mask=false）这一位必须是 0，
    // 客户端必须是 1 —— 这是两个角色在帧格式上唯一的区别。
    const char maskBit = mask ? static_cast<char>(0x80) : static_cast<char>(0x00);
    if (len < 126) {
        frame.append(static_cast<char>(maskBit | len));
    } else if (len <= 0xFFFF) {
        frame.append(static_cast<char>(maskBit | 126));
        frame.append(static_cast<char>((len >> 8) & 0xFF));
        frame.append(static_cast<char>(len & 0xFF));
    } else {
        frame.append(static_cast<char>(maskBit | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.append(static_cast<char>((static_cast<quint64>(len) >> shift) & 0xFF));
    }

    if (!mask) {
        frame += payload;
        return frame;
    }

    char maskKey[4];
    for (char &byte : maskKey)
        byte = static_cast<char>(QRandomGenerator::system()->bounded(256));
    frame.append(maskKey, 4);

    for (qsizetype i = 0; i < len; ++i)
        frame.append(static_cast<char>(payload.at(i) ^ maskKey[i % 4]));
    return frame;
}

inline QByteArray closePayload(int code, const QString &reason)
{
    QByteArray payload;
    payload.append(static_cast<char>((code >> 8) & 0xFF));
    payload.append(static_cast<char>(code & 0xFF));
    // 关闭帧载荷上限 125 字节，去掉 2 字节状态码后原因最多 123 字节。
    // 截断可能切在 UTF-8 字符中间：按前导字节算出每个字符的字节数，只保留完整字符。
    QByteArray reasonBytes = reason.toUtf8();
    if (reasonBytes.size() > 123) {
        int cut = 0;
        while (cut < 123 && cut < reasonBytes.size()) {
            const unsigned char lead = static_cast<unsigned char>(reasonBytes.at(cut));
            int need = 1;
            if ((lead & 0xE0) == 0xC0)
                need = 2;
            else if ((lead & 0xF0) == 0xE0)
                need = 3;
            else if ((lead & 0xF8) == 0xF0)
                need = 4;
            if (cut + need > 123)
                break;   // 这个字符放不下，整字丢掉，不留半个
            cut += need;
        }
        reasonBytes.truncate(cut);
    }
    payload += reasonBytes;
    return payload;
}

inline bool FrameReader::next(Frame *frame)
{
    if (!m_error.isEmpty() || frame == nullptr)
        return false;
    if (m_buffer.size() < 2)
        return false;

    const auto *bytes = reinterpret_cast<const unsigned char *>(m_buffer.constData());
    const bool fin       = (bytes[0] & 0x80) != 0;
    const bool hasRsv    = (bytes[0] & 0x70) != 0;
    const int  opcode    = bytes[0] & 0x0F;
    const bool masked    = (bytes[1] & 0x80) != 0;
    quint64    length    = bytes[1] & 0x7F;
    qsizetype  offset    = 2;

    if (hasRsv) {
        m_error = QStringLiteral("帧里带了 RSV 扩展位，但本工具没有协商任何扩展");
        return false;
    }
    switch (opcode) {
    case OpContinuation:
    case OpText:
    case OpBinary:
    case OpClose:
    case OpPing:
    case OpPong:
        break;
    default:
        m_error = QStringLiteral("收到未知的帧类型 0x%1").arg(opcode, 0, 16);
        return false;
    }

    if (length == 126) {
        if (m_buffer.size() < 4)
            return false;
        length = (static_cast<quint64>(bytes[2]) << 8) | bytes[3];
        offset = 4;
    } else if (length == 127) {
        if (m_buffer.size() < 10)
            return false;
        quint64 value = 0;
        for (int i = 2; i < 10; ++i)
            value = (value << 8) | bytes[i];
        if (value & 0x8000000000000000ull) {
            m_error = QStringLiteral("64 位长度字段最高位不为 0，帧格式非法");
            return false;
        }
        length = value;
        offset = 10;
    }

    const bool control = (opcode & 0x08) != 0;
    if (control && (!fin || length > 125)) {
        m_error = QStringLiteral("控制帧不能分片且载荷不得超过 125 字节");
        return false;
    }
    if (m_expectMasked) {
        // 服务端角色：客户端发来的帧必须带掩码（RFC 6455 §5.1）。
        // 不带掩码说明对端不是标准 WebSocket 实现（常见于手写脚本或代理改写），
        // 按规范应当用 1002 关闭，这里先给出可读原因由上层断开。
        if (!masked) {
            m_error = QStringLiteral("客户端发来的帧没有掩码，不符合 RFC 6455");
            return false;
        }
    } else if (masked) {
        // 客户端角色：服务端发来的帧禁止加掩码（加了说明对端实现有问题，
        // 也可能是中间设备把流量改写成了别的协议）
        m_error = QStringLiteral("服务端发来的帧带了掩码，不符合 RFC 6455");
        return false;
    }
    if (length > kMaxMessageBytes) {
        m_error = QStringLiteral("单帧 %1 字节，超过 %2 字节上限，已拒绝")
                      .arg(length)
                      .arg(kMaxMessageBytes);
        return false;
    }

    // 掩码键紧跟在长度字段后面，占 4 字节；载荷起点要跳过它
    const qsizetype maskSize = masked ? 4 : 0;
    const quint64 needed = static_cast<quint64>(offset + maskSize) + length;
    if (static_cast<quint64>(m_buffer.size()) < needed)
        return false;   // 还没收全

    frame->opcode = opcode;
    frame->fin    = fin;
    if (masked) {
        const auto *maskKey = bytes + offset;
        QByteArray payload = m_buffer.mid(offset + maskSize, static_cast<qsizetype>(length));
        for (qsizetype i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(payload.at(i) ^ maskKey[i % 4]);
        frame->payload = payload;
    } else {
        frame->payload = m_buffer.mid(offset, static_cast<qsizetype>(length));
    }
    m_buffer.remove(0, static_cast<qsizetype>(needed));
    return true;
}

inline HttpHead parseHttpHead(const QByteArray &data)
{
    HttpHead head;
    const int end = data.indexOf("\r\n\r\n");
    if (end < 0) {
        // 16 KB 还收不到头部结束标记，多半不是 HTTP（例如连到了别的端口）
        if (data.size() > 16 * 1024)
            head.error = QStringLiteral("响应头超过 16 KB 仍未结束，对方可能不是 WebSocket 服务");
        return head;
    }
    head.length = end + 4;

    const QList<QByteArray> lines = data.left(end).split('\n');
    if (lines.isEmpty()) {
        head.error = QStringLiteral("响应为空");
        return head;
    }

    const QByteArray statusLine = lines.first().trimmed();
    const QList<QByteArray> parts = statusLine.simplified().split(' ');
    if (parts.size() < 2 || !parts.first().startsWith("HTTP/")) {
        head.error = QStringLiteral("对方返回的不是 HTTP 响应：%1")
                         .arg(QString::fromLatin1(statusLine.left(60)));
        return head;
    }
    bool ok = false;
    head.status = parts.at(1).toInt(&ok);
    if (!ok) {
        head.error = QStringLiteral("HTTP 状态码无法解析：%1").arg(QString::fromLatin1(parts.at(1)));
        return head;
    }

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;   // 畸形头直接跳过，不影响 101 的判定
        const QString name = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        const QString value = QString::fromLatin1(line.mid(colon + 1)).trimmed();
        head.headers.insert(name, value);
    }

    head.complete = true;
    return head;
}

inline HttpRequest parseHttpRequest(const QByteArray &data)
{
    HttpRequest req;
    const int end = data.indexOf("\r\n\r\n");
    if (end < 0) {
        if (data.size() > 16 * 1024)
            req.error = QStringLiteral("请求头超过 16 KB 仍未结束，对方可能不是 WebSocket 客户端");
        return req;
    }
    req.length = end + 4;

    const QList<QByteArray> lines = data.left(end).split('\n');
    if (lines.isEmpty()) {
        req.error = QStringLiteral("请求为空");
        return req;
    }

    const QByteArray requestLine = lines.first().trimmed();
    const QList<QByteArray> parts = requestLine.simplified().split(' ');
    if (parts.size() < 3 || !parts.last().startsWith("HTTP/")) {
        req.error = QStringLiteral("请求行不是 HTTP 格式：%1")
                        .arg(QString::fromLatin1(requestLine.left(60)));
        return req;
    }
    req.method      = QString::fromLatin1(parts.at(0)).toUpper();
    req.target      = QString::fromLatin1(parts.at(1));
    req.httpVersion = QString::fromLatin1(parts.at(2).mid(5));

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;   // 畸形头直接跳过：缺一个可选头不影响握手，缺必须项由校验函数报
        const QString name = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        const QString value = QString::fromLatin1(line.mid(colon + 1)).trimmed();
        req.headers.insert(name, value);
    }

    req.complete = true;
    return req;
}

/// 头值里是否含某个 token（`Connection: keep-alive, Upgrade` 这类逗号列表）。
/// RFC 明确要求按 token 比较而不是整串相等，否则 `keep-alive, Upgrade` 会被误判。
inline bool headerHasToken(const QMap<QString, QString> &headers,
                           const char *name, const char *token)
{
    const QString value = headers.value(QString::fromLatin1(name));
    const QStringList items = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &item : items) {
        if (item.trimmed().compare(QLatin1String(token), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

inline QString validateHandshake(const HttpRequest &req, int *status)
{
    const auto setStatus = [status](int code) {
        if (status != nullptr)
            *status = code;
    };

    if (!req.error.isEmpty()) {
        setStatus(400);
        return req.error;
    }
    if (req.method != QLatin1String("GET")) {
        setStatus(405);
        return QStringLiteral("握手请求必须是 GET，收到 %1").arg(req.method);
    }
    if (req.httpVersion != QLatin1String("1.1")) {
        setStatus(400);
        return QStringLiteral("握手必须用 HTTP/1.1，收到 HTTP/%1").arg(req.httpVersion);
    }
    if (!req.target.startsWith(QLatin1Char('/'))) {
        setStatus(400);
        return QStringLiteral("请求路径不合法：%1").arg(req.target);
    }
    if (!headerHasToken(req.headers, "upgrade", "websocket")) {
        setStatus(400);
        return QStringLiteral("缺少 `Upgrade: websocket`：这个端口只接 WebSocket 握手，"
                              "浏览器直接打开只会看到这段说明");
    }
    if (!headerHasToken(req.headers, "connection", "upgrade")) {
        setStatus(400);
        return QStringLiteral("缺少 `Connection: Upgrade`");
    }
    const QString version = req.headers.value(QStringLiteral("sec-websocket-version")).trimmed();
    if (version != QLatin1String("13")) {
        setStatus(426);
        return version.isEmpty()
                   ? QStringLiteral("缺少 `Sec-WebSocket-Version: 13`")
                   : QStringLiteral("只支持 Sec-WebSocket-Version: 13，客户端发的是 %1").arg(version);
    }

    const QByteArray key = req.headers.value(QStringLiteral("sec-websocket-key")).toLatin1();
    if (key.isEmpty()) {
        setStatus(400);
        return QStringLiteral("缺少 `Sec-WebSocket-Key`");
    }
    // RFC 要求 Key 是 16 字节随机数的 base64。用 AbortOnBase64DecodingErrors 而不是
    // 宽松解码：宽松模式会把乱码当合法输入吞掉，回一个 101 之后立刻收到一堆帧错误。
    const auto decoded = QByteArray::fromBase64Encoding(
        key, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != 16) {
        setStatus(400);
        return QStringLiteral("`Sec-WebSocket-Key` 必须是 16 字节随机数的 base64");
    }
    return QString();
}

inline QString selectProtocol(const HttpRequest &req, const QStringList &supported, QString *error)
{
    if (error != nullptr)
        error->clear();

    QStringList offered;
    const QStringList raw = req.headers.value(QStringLiteral("sec-websocket-protocol"))
                                .split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &item : raw) {
        const QString trimmed = item.trimmed();
        if (!trimmed.isEmpty() && !offered.contains(trimmed, Qt::CaseInsensitive))
            offered << trimmed;
    }

    if (offered.isEmpty())
        return QString();   // 客户端没提协议，就不回这个头
    if (supported.isEmpty())
        return offered.first();   // 用户没配：回显客户端第一个，保证能连上

    for (const QString &candidate : offered) {
        for (const QString &allowed : supported) {
            if (candidate.compare(allowed, Qt::CaseInsensitive) == 0)
                return allowed;
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("客户端要求的子协议（%1）都不在服务端配置（%2）里")
                     .arg(offered.join(QStringLiteral(", ")), supported.join(QStringLiteral(", ")));
    }
    return QString();
}

inline QByteArray buildAcceptResponse(const QByteArray &clientKey, const QString &protocol)
{
    QByteArray out = "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Accept: " + acceptFor(clientKey) + "\r\n";
    if (!protocol.isEmpty())
        out += "Sec-WebSocket-Protocol: " + protocol.toUtf8() + "\r\n";
    out += "\r\n";
    return out;
}

inline QByteArray buildRejectResponse(int status, const QString &message)
{
    const char *reason = "Bad Request";
    if (status == 405)
        reason = "Method Not Allowed";
    else if (status == 426)
        reason = "Upgrade Required";
    else if (status == 503)
        reason = "Service Unavailable";

    QByteArray body = message.toUtf8();
    body += "\n";   // 末尾补换行：用 curl 或浏览器看时不会和别人给的提示黏在一行

    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
    out += "Content-Type: text/plain; charset=utf-8\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n";
    if (status == 426)
        out += "Sec-WebSocket-Version: 13\r\n";
    out += "\r\n";
    out += body;
    return out;
}

inline QString closeCodeText(int code)
{
    switch (code) {
    case 1000: return QStringLiteral("正常关闭");
    case 1001: return QStringLiteral("端点离开（服务端关闭或浏览器跳走）");
    case 1002: return QStringLiteral("协议错误");
    case 1003: return QStringLiteral("不支持的数据类型");
    case 1006: return QStringLiteral("异常断开（没有收到关闭帧）");
    case 1007: return QStringLiteral("文本不是合法的 UTF-8");
    case 1008: return QStringLiteral("违反服务端策略");
    case 1009: return QStringLiteral("消息超过服务端上限");
    case 1010: return QStringLiteral("客户端要求了服务端不支持的扩展");
    case 1011: return QStringLiteral("服务端内部错误");
    case 1012: return QStringLiteral("服务端正在重启");
    case 1013: return QStringLiteral("服务端稍后重试");
    case 1015: return QStringLiteral("TLS 握手失败");
    default:
        break;
    }
    if (code >= 3000 && code <= 3999)
        return QStringLiteral("应用自定义关闭码");
    if (code >= 4000 && code <= 4999)
        return QStringLiteral("私有关闭码");
    return QStringLiteral("未知关闭码");
}

} // namespace wsproto

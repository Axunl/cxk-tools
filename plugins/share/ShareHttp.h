#pragma once

// ---------------------------------------------------------------------------
// 文件共享插件的 HTTP 基础工具。
//
// 这里只放**纯逻辑**：请求头解析、URL 解码、Content-Disposition 编码、MIME 推断、
// 本机网卡地址枚举、大小格式化。全部是纯字符串 / 纯枚举处理，不碰 socket、不碰界面。
//
// 为什么单独抽成 header：AGENTS.md 规则 2 要求改动必须有实际跑过的验证，而仓库不留
// 测试工程。抽成 inline header 后，临时断言程序只要 include 这个文件就能验证
// 「中文文件名不会乱码」「HTTP/1.0 不带 keep-alive 要断开」这类边界，
// 不必真的起服务、开浏览器点一遍。
// ---------------------------------------------------------------------------

#include <QAbstractSocket>
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMimeDatabase>
#include <QMimeType>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QString>
#include <QUrl>

#include <algorithm>

namespace share {

/// 一个可被局域网其他设备访问的入口地址。
struct LocalAddress
{
    QString url;     ///< http://192.168.1.7:5421
    QString ip;      ///< 192.168.1.7
    QString nicName; ///< 网卡名（WLAN / VMware Network Adapter VMnet8 ...），参与排序判断
    QString label;   ///< "WLAN · 192.168.1.7"，给地址下拉菜单显示
};

/// HTTP 状态码对应的标准原因短语。只列本项目会真正用到的几个。
inline QString httpStatusText(int code)
{
    switch (code) {
    case 200: return QStringLiteral("OK");
    case 204: return QStringLiteral("No Content");
    case 206: return QStringLiteral("Partial Content");
    case 400: return QStringLiteral("Bad Request");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 413: return QStringLiteral("Payload Too Large");
    case 431: return QStringLiteral("Request Header Fields Too Large");
    case 500: return QStringLiteral("Internal Server Error");
    default:  return QStringLiteral("Error");
    }
}

///
/// percent-decode 一段字节。
///
/// 刻意不用 QUrlQuery：它会把 '+' 解析成空格，而路径与文件名里的 '+' 应当保持原义
/// （比如「C++ 笔记.txt」经前端 encodeURIComponent 后是 %2B，解码后必须是 '+'）。
///
inline QString percentDecode(const QByteArray &raw)
{
    return QUrl::fromPercentEncoding(raw);
}

/// 解析结果。字段全部是 Qt 值类型，便于在临时程序里构造与断言。
struct RequestHead
{
    QString method;                  ///< 已大写：GET / HEAD / POST / DELETE
    QString path;                    ///< percent-decode 过的路径，不含 query
    QString query;                   ///< '?' 之后的部分（本插件暂未使用，保留给前端加缓存参数）
    QString version;                 ///< HTTP/1.1 等
    QHash<QString, QString> headers; ///< 键统一转小写，取值时不用再关心大小写
    bool valid = false;              ///< 请求行是否可用（畸形请求直接回 400，不进路由）

    /// 按小写键取头。缺失时返回 fallback。
    QString header(const QString &lowerKey, const QString &fallback = QString()) const
    {
        return headers.value(lowerKey, fallback);
    }
};

///
/// 解析一段完整的请求头（调用方保证已按 "\r\n\r\n" 切好、不含末尾空行）。
///
/// 容错取向：不认识的头直接忽略、重复的键后者覆盖（浏览器不会发重复键，
/// 真发了说明对端不是浏览器，本插件也没必要讲究）。
///
inline RequestHead parseRequestHead(const QByteArray &raw)
{
    RequestHead head;
    const QList<QByteArray> lines = raw.split('\n');
    if (lines.isEmpty())
        return head;

    const QList<QByteArray> firstLine = lines.first().trimmed().split(' ');
    if (firstLine.size() < 2)
        return head;   // 连 "METHOD /path" 都凑不齐，交给调用方回 400

    head.method = QString::fromLatin1(firstLine.at(0)).toUpper();
    if (firstLine.size() >= 3)
        head.version = QString::fromLatin1(firstLine.at(2)).trimmed();

    // 目标可能是绝对 URL（代理风格），也可能被整体 percent-encode 过（少见但合法）。
    // 只用它的 path 部分：本服务只处理本机路径，不替任何人转发。
    const QString target = QString::fromUtf8(firstLine.at(1));
    const int queryPos = target.indexOf(QLatin1Char('?'));
    const QString rawPath = queryPos >= 0 ? target.left(queryPos) : target;
    head.query = queryPos >= 0 ? target.mid(queryPos + 1) : QString();

    // 解码前先剥掉"绝对 URL"的外壳（scheme://host 部分），否则路径会带上主机名。
    int pathStart = rawPath.indexOf(QLatin1Char('/'));
    if (pathStart < 0)
        return head;
    head.path = percentDecode(rawPath.mid(pathStart).toUtf8());

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;   // 没有冒号或有 :// 之前的怪东西，忽略而不是整体失败
        const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
        const QString value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        if (!key.isEmpty())
            head.headers.insert(key, value);
    }

    head.valid = !head.method.isEmpty() && head.path.startsWith(QLatin1Char('/'));
    return head;
}

///
/// 这个连接在一次请求结束后能不能继续复用。
///
/// 规则按 RFC 9112：HTTP/1.1 默认复用，除非明确 Connection: close；
/// HTTP/1.0 默认断开，只有明确 keep-alive 才复用。判断错了会让浏览器要么
/// 收到一堆 "Connection reset"，要么每次请求都重新握手。
///
inline bool wantsKeepAlive(const RequestHead &head)
{
    const QString connection = head.header(QStringLiteral("connection")).toLower();
    if (connection.contains(QStringLiteral("close")))
        return false;
    if (head.version.compare(QLatin1String("HTTP/1.0"), Qt::CaseInsensitive) == 0)
        return connection.contains(QStringLiteral("keep-alive"));
    return true;
}

///
/// 下载响应里的 Content-Disposition。
///
/// 中文文件名必须写两种形式：`filename=` 只认 ASCII（浏览器实际按 latin1 解码，
/// 直接塞 UTF-8 会变成乱码），`filename*=UTF-8''...` 才是 RFC 5987 的正解。
/// 两个都给，现代浏览器优先用后者，老客户端退化成下划线占位而不是乱码。
///
inline QByteArray contentDispositionAttachment(const QString &fileName)
{
    QString fallback;
    fallback.reserve(fileName.size());
    for (const QChar ch : fileName) {
        const ushort code = ch.unicode();
        const bool printable = code >= 0x20 && code < 0x7F;
        const bool reserved = ch == QLatin1Char('"') || ch == QLatin1Char('\\');
        fallback.append(printable && !reserved ? ch : QLatin1Char('_'));
    }
    const QByteArray encoded = QUrl::toPercentEncoding(fileName);
    return "Content-Disposition: attachment; filename=\"" + fallback.toLatin1()
           + "\"; filename*=UTF-8''" + encoded;
}

/// 按扩展名推断 MIME。推断不出来时给 octet-stream，让浏览器走下载而不是猜成文本。
inline QByteArray mimeTypeForName(const QString &fileName)
{
    const QMimeType type = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);
    const QByteArray name = type.name().toLatin1();
    return name.isEmpty() ? QByteArrayLiteral("application/octet-stream") : name;
}

/// 人类可读的文件大小。1 位小数的 KB/MB 只在数值较小时用，避免 "1024.0 MB" 这种冗余。
inline QString humanSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', kb < 10.0 ? 1 : 0);
    const double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', mb < 10.0 ? 1 : 0);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
}

///
/// 网卡名看起来像虚拟适配器吗？
///
/// 为什么需要这个判断：多网卡机器上，VMware / Hyper-V / WSL / Docker 的虚拟适配器
/// 也会有一个 192.168.x 地址，而且往往比真实网卡先枚举出来。把它们排在前面，
/// 用户复制走的地址手机根本连不上 —— 这是实测踩到的（VMnet8 抢了第一位）。
///
inline bool looksLikeVirtualNic(const QString &name)
{
    static const char *const kMarkers[] = {
        "vmware",   "vmnet",      "virtualbox", "vbox",     "hyper-v",
        "vethernet", "wsl",       "docker",     "veth",     "virbr",
        "br-",      "tap-",       "tap0",       "tun0",     "utun",
        "tailscale", "zerotier",  "loopback",   "bluetooth"};
    const QString lower = name.toLower();
    for (const char *marker : kMarkers) {
        if (lower.contains(QLatin1String(marker)))
            return true;
    }
    return false;
}

///
/// 本机所有可用于访问本服务的 IPv4 地址，按「最可能是访客该用的那个」排序。
///
/// 为什么只发 IPv4：IPv6 的 URL 要写成 http://[fe80::1%eth0]:5421 —— 带网卡作用域、
/// 要方括号、还要对端也在同一链路，让用户在浏览器里手输基本必错。
/// 本服务 bind 的也是 AnyIPv4，发出去的地址必须是真能连上的那些。
///
inline QList<LocalAddress> localAddresses(quint16 port)
{
    QList<LocalAddress> result;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();

    for (const QNetworkInterface &iface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        // IsRunning 与 IsUp 都要判：拔了网线的网卡 IsUp 仍可能为真（Windows 上常见）
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning))
            continue;
        if (flags & QNetworkInterface::IsLoopBack)
            continue;

        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            const quint32 raw = address.toIPv4Address();
            if (raw == 0)
                continue;
            // 169.254/16 是链路本地自动地址（DHCP 没拿到租约时出现），同网段其他机器连不上
            if ((raw & 0xFFFF0000u) == 0xA9FE0000u)
                continue;

            LocalAddress item;
            item.ip = address.toString();
            item.url = QStringLiteral("http://%1:%2").arg(item.ip).arg(port);
            item.nicName = iface.humanReadableName();
            item.label = item.nicName.isEmpty()
                             ? item.ip
                             : QStringLiteral("%1 · %2").arg(item.nicName, item.ip);
            result.append(item);
        }
    }

    // 排序三条启发式（都来自实际场景，不是理论最优）：
    //   1) 192.168 / 10.x / 172.16-31 是家用与办公网络的常见段，公有段地址多半是
    //      校园网或运营商直连，仍可用但优先级低；
    //   2) 虚拟网卡（VMware / Hyper-V / WSL / Docker）排在真网卡之后；
    //   3) 宿主机的虚拟适配器几乎都占网关位 x.x.x.1，真实的 DHCP 地址很少是 .1。
    const auto rank = [](const LocalAddress &address) {
        int score = 0;
        const quint32 raw = QHostAddress(address.ip).toIPv4Address();
        if ((raw & 0xFFFF0000u) == 0xC0A80000u)        // 192.168.0.0/16
            score += 0;
        else if ((raw & 0xFF000000u) == 0x0A000000u)   // 10.0.0.0/8
            score += 1;
        else if ((raw & 0xFFF00000u) == 0xAC100000u)   // 172.16.0.0/12
            score += 2;
        else
            score += 3;
        if (looksLikeVirtualNic(address.nicName))
            score += 8;
        if ((raw & 0xFFu) == 1u)
            score += 4;
        return score;
    };

    std::stable_sort(result.begin(), result.end(),
                     [&rank](const LocalAddress &a, const LocalAddress &b) {
                         const int ra = rank(a);
                         const int rb = rank(b);
                         if (ra != rb)
                             return ra < rb;
                         return a.ip < b.ip;   // 同权重按 IP 排，保证每次顺序一致
                     });
    return result;
}

} // namespace share

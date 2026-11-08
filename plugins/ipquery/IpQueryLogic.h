#pragma once

// ---------------------------------------------------------------------------
// IP 查询的纯逻辑层：本机网卡地址收集 + 公网接口响应解析。
//
// 刻意不依赖任何 UI 头文件，只用 Qt Core/Network 的值类型，
// 这样 AGENTS.md 规则 2 要求的临时断言程序可以直接 include 本文件验证，
// 而不是把代码复制出去测（复制会测到旧版本）。
//
// 公网数据源两个（面板里按序降级，见 IpQueryTool.cpp）：
//   1. https://ipwho.is/  —— HTTPS，返回 ip/city/region/success/message
//   2. http://ip-api.com/  —— 免费接口仅 HTTP，返回 query/city/regionName/status/message
// 字段名不同，parsePublicIpJson() 统一归一化，不让 UI 层感知差异。
// ---------------------------------------------------------------------------

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QString>

namespace ipquery {

/// 一条本机网卡地址记录。
struct NicAddress
{
    QString ip;       ///< 点分十进制 IPv4 文本
    QString nicName;  ///< 网卡展示名（Windows 友好名 / Linux 接口名）
};

///
/// 收集本机可用的 IPv4 地址，过滤规则（与参考实现的展示口径一致）：
///  - 只要 IPv4：IPv6 地址长且用户很少手抄，本工具定位是"看一眼、点一下就复制"；
///  - 排除回环（IsLoopback 接口 / 127.x）；
///  - 排除链路本地 169.254.x（DHCP 失败时系统自分配，不是用户想要的地址）；
///  - 接口必须处于 Up 状态，拔掉的网卡不展示。
/// 顺序保持系统枚举顺序，不做重排（与系统网络设置面板一致，用户好对照）。
///
inline QList<NicAddress> collectLocalIpv4()
{
    QList<NicAddress> out;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!(iface.flags() & QNetworkInterface::IsUp))
            continue;
        // Qt 6 的枚举名是 IsLoopBack（大写 B），别按 IsLoopback 猜
        if (iface.flags() & QNetworkInterface::IsLoopBack)
            continue;

        // humanReadableName 在 Windows 上就是"VMware Network Adapter VMnet1"
        // 这类友好名，Linux/macOS 上等价于接口名（eth0 / wlan0）；极端情况下
        // 为空则退回原始 name()，不让来源行空着。
        QString name = iface.humanReadableName();
        if (name.isEmpty())
            name = iface.name();

        // ipAddresses() 在 Qt 6 里不存在，只有 addressEntries()，
        // 每个 entry 通过 ip() 取地址
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            if (addr.isLoopback())
                continue;
            // 169.254.0.0/16 = 链路本地（APIPA），没有路由意义
            const quint32 v4 = addr.toIPv4Address();
            if ((v4 & 0xFFFF0000u) == 0xA9FE0000u)
                continue;

            NicAddress item;
            item.ip = addr.toString();
            item.nicName = name;
            out.append(item);
        }
    }
    return out;
}

///
/// 拼接归属地文案："城市 地区"（如 "Tokyo Tokyo"）。
/// 两者都空返回空串（调用方据此省略位置行）；只有一边时返回那一边。
/// 不做本地化改写（Guangzhou Guangdong 原样展示），与数据源保持一致。
///
inline QString formatLocation(const QString &city, const QString &region)
{
    const QString c = city.trimmed();
    const QString r = region.trimmed();
    if (c.isEmpty() && r.isEmpty())
        return QString();
    if (c.isEmpty())
        return r;
    if (r.isEmpty())
        return c;
    return c + QLatin1Char(' ') + r;
}

///
/// 解析公网接口返回的 JSON，兼容两种已知形态：
///   ipwho.is:  {"ip":"x","success":true,"city":"a","region":"b",...}
///   ip-api:    {"status":"success","query":"x","city":"a","regionName":"b",...}
/// 成功返回 true 并填 *ip / *location（location 可能为空）；
/// 失败（接口报错 / 格式不对 / 非 JSON）返回 false 并填 *error 供界面展示。
/// 两个出参指针允许传 nullptr，纯探针调用时可以只要返回值。
///
inline bool parsePublicIpJson(const QByteArray &body, QString *ip,
                              QString *location, QString *error)
{
    const auto setError = [error](const QString &msg) {
        if (error)
            *error = msg;
    };

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // 公网接口被劫持/超时兜底页时会返回 HTML，这里统一按"非 JSON"报错
        setError(QStringLiteral("接口返回了无法解析的内容"));
        return false;
    }

    const QJsonObject obj = doc.object();
    QString rawIp, city, region;

    if (obj.contains(QStringLiteral("success"))) {
        // ---- ipwho.is 形态 ----
        if (!obj.value(QStringLiteral("success")).toBool()) {
            setError(obj.value(QStringLiteral("message")).toString(
                QStringLiteral("接口返回失败")));
            return false;
        }
        rawIp = obj.value(QStringLiteral("ip")).toString();
        city = obj.value(QStringLiteral("city")).toString();
        region = obj.value(QStringLiteral("region")).toString();
    } else if (obj.contains(QStringLiteral("status"))) {
        // ---- ip-api.com 形态 ----
        if (obj.value(QStringLiteral("status")).toString()
            != QStringLiteral("success")) {
            setError(obj.value(QStringLiteral("message")).toString(
                QStringLiteral("接口返回失败")));
            return false;
        }
        // ip-api 的地址字段叫 query，地区字段叫 regionName
        rawIp = obj.value(QStringLiteral("query")).toString();
        city = obj.value(QStringLiteral("city")).toString();
        region = obj.value(QStringLiteral("regionName")).toString();
    } else {
        // 两种已知形态都对不上：接口改版了，明确报错而不是猜字段
        setError(QStringLiteral("接口返回了未知的数据格式"));
        return false;
    }

    // 地址字段缺失视为失败：宁可报错也不要拿空 IP 蒙混成一条记录
    if (rawIp.trimmed().isEmpty()) {
        setError(QStringLiteral("接口返回的数据里没有 IP 地址"));
        return false;
    }

    if (ip)
        *ip = rawIp.trimmed();
    if (location)
        *location = formatLocation(city, region);
    return true;
}

} // namespace ipquery

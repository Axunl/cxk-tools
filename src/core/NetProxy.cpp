#include "NetProxy.h"

#include "Settings.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QStringList>
#include <QTcpSocket>

namespace {

/// 本机常见代理客户端的默认入站端口。顺序就是命中优先级：
/// Clash 系（7890 / 7897）在国内最常见，v2rayN 次之，Shadowsocks 最后。
/// 说明：7890 / 7897 / 10809 是「HTTP + SOCKS 混合端口」，按 HTTP 用即可；
/// 7891 / 10808 / 1080 是纯 SOCKS5。
struct LocalCandidate
{
    quint16 port;
    QNetworkProxy::ProxyType type;
    const char *name;
};

constexpr LocalCandidate kLocalCandidates[] = {
    {7890, QNetworkProxy::HttpProxy, "Clash"},
    {7897, QNetworkProxy::HttpProxy, "Clash Verge"},
    {10809, QNetworkProxy::HttpProxy, "v2rayN"},
    {7891, QNetworkProxy::Socks5Proxy, "Clash"},
    {10808, QNetworkProxy::Socks5Proxy, "v2rayN"},
    {1080, QNetworkProxy::Socks5Proxy, "Shadowsocks"},
};

/// 端口探测的建连超时。localhost 只有两种结果：立刻连上、或立刻被拒（RST）。
/// 120ms 只是兜住"被防火墙静默丢包"这种异常情况。
constexpr int kProbeConnectMs = 120;
/// 握手回包的等待时间。真正的代理就在本机，回一行字节是毫秒级的。
constexpr int kProbeReplyMs = 300;
/// 整轮探测的时间预算：六个端口全都不通时，也别让第一次检查干等小一秒。
constexpr int kProbeBudgetMs = 450;

/// 解析缓存。用文件级静态量：解析结果跟着"设置 + 本机环境"走，与任何对象实例无关。
QBasicMutex g_mutex;
bool g_resolved = false;
NetProxy::Choice g_choice;

/// 读一组环境变量里第一个非空值；命中的名字一并带出（日志要写清是哪个变量生效）。
QString firstEnv(const QStringList &names, QString *hit = nullptr)
{
    for (const QString &name : names) {
        const QString value = qEnvironmentVariable(name.toUtf8().constData()).trimmed();
        if (!value.isEmpty()) {
            if (hit)
                *hit = name;
            return value;
        }
    }
    return QString();
}

/// 读满 want 个字节（或超时）。代理的应答很小，但 TCP 不保证一次 readyRead 就能拿到
/// 完整应答，只 read 一次会偶发地把"真是代理"判成"不是代理"。
QByteArray readAtLeast(QTcpSocket &socket, int want, int timeoutMs)
{
    QByteArray data;
    QElapsedTimer timer;
    timer.start();
    while (data.size() < want) {
        const int left = timeoutMs - int(timer.elapsed());
        if (left <= 0 || !socket.waitForReadyRead(left))
            break;
        data += socket.read(want - data.size());
    }
    return data;
}

/// 这个端口上是不是一个能用的 HTTP 代理。
///
/// 只 connect 成功不足以判断：7890 也可能被别的开发服务占着，那样把更新流量全塞过去
/// 会比不改还糟。所以真的发一次 CONNECT 握手——任何 HTTP 代理都会先回一行状态
/// （200 / 403 / 407 都算，说明它确实是代理），不会等到隧道建起来。
/// 用 .invalid 域名（保留域，永远解析不到）保证这次探测不会产生真实流量。
bool speaksHttpProxy(quint16 port)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(kProbeConnectMs))
        return false;

    socket.write("CONNECT example.invalid:443 HTTP/1.1\r\nHost: example.invalid:443\r\n\r\n");
    const QByteArray head = readAtLeast(socket, 5, kProbeReplyMs);
    socket.abort();
    return head.startsWith("HTTP/");
}

/// 这个端口上是不是一个 SOCKS5 服务。
/// 发一次 SOCKS5 问候（VER=5, 一种方法：无认证），回包首字节是 0x05 即确认；
/// 0xFF（方法不被接受）也算——那只是要认证，协议本身没错。
bool speaksSocks5(quint16 port)
{
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(kProbeConnectMs))
        return false;

    socket.write(QByteArray::fromHex("050100"));
    const QByteArray head = readAtLeast(socket, 2, kProbeReplyMs);
    socket.abort();
    return head.size() == 2 && quint8(head.at(0)) == 0x05;
}

} // namespace

// ---------------------------------------------------------------- 对外入口

NetProxy::Choice NetProxy::forUrl(const QUrl &url)
{
    // NO_PROXY 优先于一切：环境变量的语义就是"这些地址别走代理"，
    // 用户既然写了就别自作聪明（企业内网镜像常见这么配）。
    // 它按目标逐个判断，所以不进缓存。
    const QString noProxy = firstEnv({QStringLiteral("no_proxy"), QStringLiteral("NO_PROXY")});
    if (matchesNoProxy(url.host(), noProxy))
        return {QNetworkProxy(QNetworkProxy::NoProxy), QStringLiteral("NO_PROXY 命中")};

    QMutexLocker<QBasicMutex> lock(&g_mutex);
    if (!g_resolved) {
        g_choice = resolve(url);
        g_resolved = true;
    }
    return g_choice;
}

NetProxy::Choice NetProxy::reload()
{
    Choice fresh;
    {
        QMutexLocker<QBasicMutex> lock(&g_mutex);
        fresh = resolve(QUrl(QStringLiteral(
            "https://github.com/Axunl/cxk-tools/releases/latest/download/latest.json")));
        g_choice = fresh;
        g_resolved = true;
    }
    // 日志放在锁外：解析可能要探测端口，别让别的线程陪着等
    qInfo().noquote() << "NetProxy: 重新解析 →" << statusText();
    return fresh;
}

void NetProxy::invalidate()
{
    QMutexLocker<QBasicMutex> lock(&g_mutex);
    g_resolved = false;
}

QString NetProxy::statusText()
{
    // 走 forUrl 而不是直接读缓存：状态行必须包含 NO_PROXY 的判定，
    // 否则用户明明写了 no_proxy、界面却显示"正在用代理"，只会更迷惑。
    const Choice choice = forUrl(QUrl(QStringLiteral(
        "https://github.com/Axunl/cxk-tools/releases/latest/download/latest.json")));
    if (choice.isDirect())
        return QStringLiteral("直连（%1）").arg(choice.source);
    return QStringLiteral("%1 %2").arg(choice.source, describe(choice.proxy));
}

// ---------------------------------------------------------------- 解析

NetProxy::Choice NetProxy::resolve(const QUrl &url)
{
    const Settings &settings = Settings::instance();
    const QString mode = settings.proxyMode();

    // 用户明确关了代理：任何自动探测都不做，这是"我在调试网络"时的逃生门
    if (mode == QLatin1String("direct"))
        return {QNetworkProxy(QNetworkProxy::NoProxy), QStringLiteral("已关闭代理")};

    if (mode == QLatin1String("manual")) {
        QNetworkProxy proxy(typeFromString(settings.proxyType()), settings.proxyHost(),
                            quint16(settings.proxyPort()));
        if (proxy.hostName().trimmed().isEmpty() || proxy.port() <= 0)
            return {QNetworkProxy(QNetworkProxy::NoProxy), QStringLiteral("手动代理没填完整，已直连")};
        return {proxy, QStringLiteral("手动代理")};
    }

    // ---- auto：环境变量 → 系统代理 → 本机端口探测 → 直连
    Choice choice = fromEnvironment(url);
    if (!choice.isDirect())
        return choice;

    choice = fromSystem(url);
    if (!choice.isDirect())
        return choice;

    choice = fromLocalPorts();
    if (!choice.isDirect())
        return choice;

    return {QNetworkProxy(QNetworkProxy::NoProxy), QStringLiteral("没找到代理")};
}

NetProxy::Choice NetProxy::fromEnvironment(const QUrl &url)
{
    // https 目标优先看 https_proxy：很多环境里 http_proxy 指向本地换皮代理、
    // 只有 https_proxy 能真出网，顺序反了会莫名其妙失败。
    // 大小写都列一遍：Windows 的环境变量名本身不敏感，Linux/macOS 是敏感的。
    QStringList names;
    if (url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0)
        names = {QStringLiteral("http_proxy"), QStringLiteral("HTTP_PROXY"), QStringLiteral("all_proxy"),
                 QStringLiteral("ALL_PROXY")};
    else
        names = {QStringLiteral("https_proxy"), QStringLiteral("HTTPS_PROXY"),
                 QStringLiteral("all_proxy"), QStringLiteral("ALL_PROXY"),
                 QStringLiteral("http_proxy"), QStringLiteral("HTTP_PROXY")};

    QString hit;
    const QString raw = firstEnv(names, &hit);
    if (raw.isEmpty())
        return {};

    const QNetworkProxy proxy = parseProxyUrl(raw);
    if (proxy.type() == QNetworkProxy::NoProxy)
        return {};
    return {proxy, QStringLiteral("环境变量 %1").arg(hit)};
}

NetProxy::Choice NetProxy::fromSystem(const QUrl &url)
{
    // Windows 读 WinINET 设置（含 PAC），macOS 走系统配置，Linux 走 KDE/GNOME 或环境变量。
    // 用户没配代理时这里返回 NoProxy，等于什么都不做。
    const QList<QNetworkProxy> list =
        QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
    for (QNetworkProxy proxy : list) {
        if (proxy.type() == QNetworkProxy::NoProxy)
            continue;
        if (proxy.hostName().isEmpty() || proxy.port() <= 0)
            continue;
        // HttpCachingProxy 是"缓存代理"，Qt 对它的支持有限，按 HTTP 代理用即可
        if (proxy.type() == QNetworkProxy::HttpCachingProxy)
            proxy.setType(QNetworkProxy::HttpProxy);
        return {proxy, QStringLiteral("系统代理")};
    }
    return {};
}

NetProxy::Choice NetProxy::fromLocalPorts()
{
    QElapsedTimer budget;
    budget.start();

    for (const LocalCandidate &candidate : kLocalCandidates) {
        if (budget.elapsed() > kProbeBudgetMs)
            break;
        const bool alive = candidate.type == QNetworkProxy::Socks5Proxy ? speaksSocks5(candidate.port)
                                                                       : speaksHttpProxy(candidate.port);
        if (!alive)
            continue;
        QNetworkProxy proxy(candidate.type, QStringLiteral("127.0.0.1"), candidate.port);
        return {proxy, QStringLiteral("本机代理（%1）").arg(QString::fromLatin1(candidate.name))};
    }
    return {};
}

// ---------------------------------------------------------------- 纯函数

QNetworkProxy NetProxy::parseProxyUrl(const QString &raw)
{
    QString text = raw.trimmed();
    if (text.isEmpty())
        return QNetworkProxy(QNetworkProxy::NoProxy);

    // 裸 "127.0.0.1:7890" 很常见（HTTP_PROXY 允许这么写），补个 scheme 才解析得动
    if (!text.contains(QLatin1String("://")))
        text.prepend(QStringLiteral("http://"));

    const QUrl url(text);
    const QString scheme = url.scheme().toLower();

    QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
    if (scheme.startsWith(QLatin1String("socks")))
        type = QNetworkProxy::Socks5Proxy;
    else if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return QNetworkProxy(QNetworkProxy::NoProxy);

    const QString host = url.host();
    if (host.isEmpty())
        return QNetworkProxy(QNetworkProxy::NoProxy);

    int port = url.port();
    if (port <= 0)
        port = type == QNetworkProxy::Socks5Proxy ? 1080 : 80;

    QNetworkProxy proxy(type, host, quint16(port));
    if (!url.userName().isEmpty()) {
        proxy.setUser(url.userName());
        proxy.setPassword(url.password());
    }
    return proxy;
}

bool NetProxy::matchesNoProxy(const QString &host, const QString &noProxy)
{
    if (host.isEmpty() || noProxy.trimmed().isEmpty())
        return false;

    const QString h = host.trimmed().toLower();
    const QStringList entries = noProxy.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString entry : entries) {
        entry = entry.trimmed().toLower();
        if (entry.isEmpty())
            continue;
        if (entry == QLatin1String("*"))
            return true;
        while (entry.startsWith(QLatin1Char('.')))
            entry.remove(0, 1);
        if (entry.isEmpty())
            continue;
        if (h == entry || h.endsWith(QStringLiteral(".") + entry))
            return true;
    }
    return false;
}

QString NetProxy::describe(const QNetworkProxy &proxy)
{
    if (proxy.type() == QNetworkProxy::NoProxy)
        return QStringLiteral("直连");

    QString kind;
    switch (proxy.type()) {
    case QNetworkProxy::Socks5Proxy:
        kind = QStringLiteral("SOCKS5");
        break;
    case QNetworkProxy::HttpProxy:
    case QNetworkProxy::HttpCachingProxy:
        kind = QStringLiteral("HTTP");
        break;
    default:
        kind = QStringLiteral("代理");
        break;
    }

    // IPv6 字面量要加方括号，否则 "2408::1:7890" 分不清哪段是端口
    const QString host = proxy.hostName().contains(QLatin1Char(':'))
                             ? QStringLiteral("[%1]").arg(proxy.hostName())
                             : proxy.hostName();
    return QStringLiteral("%1 %2:%3").arg(kind, host).arg(proxy.port());
}

QString NetProxy::typeToString(QNetworkProxy::ProxyType type)
{
    return type == QNetworkProxy::Socks5Proxy ? QStringLiteral("socks5") : QStringLiteral("http");
}

QNetworkProxy::ProxyType NetProxy::typeFromString(const QString &raw)
{
    return raw.trimmed().toLower().startsWith(QLatin1String("socks"))
               ? QNetworkProxy::Socks5Proxy
               : QNetworkProxy::HttpProxy;
}

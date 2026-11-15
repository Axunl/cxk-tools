#pragma once

// ---------------------------------------------------------------------------
// 「检查更新该走哪个代理」的解析器（core 层，不认识任何界面）。
//
// 只服务应用内更新这一条链路（UpdateChecker 把自己的 QNetworkAccessManager
// 交给它配代理），刻意**不**装成应用级 QNetworkProxyFactory：
// 那会连带改掉插件里 HTTP 调试、WebSocket 测试、局域网共享的连接行为，
// 而"更新走代理"是用户的明确诉求，别的工具该怎么连还是怎么连。
//
// 解析顺序（auto，默认）：
//   环境变量 https_proxy/http_proxy/all_proxy → 系统代理（WinINET/PAC、macOS、
//   KDE/GNOME）→ 本机常见代理端口探测（Clash 7890/7897、v2rayN 10809/10808…）→ 直连。
// 为什么最后那步要探测端口：国内大多数人开着 Clash，但没开「系统代理」（只开 TUN，
// 或者在浏览器插件里配）。这时系统 API 查不到任何代理，程序直连 GitHub 就是几十 KB/s
// 甚至完全不动——用户看到的就是"更新永远下不完"。2026-11-15 就是这么踩的。
// ---------------------------------------------------------------------------

#include <QNetworkProxy>
#include <QString>
#include <QUrl>

class NetProxy
{
public:
    /// 解析结果：代理本身 + 它从哪来（写日志/界面用）。
    struct Choice
    {
        /// 必须显式初值成 NoProxy：QNetworkProxy 的默认构造是 DefaultProxy（"去问别人"），
        /// 而 Choice 是"最终结论"，空构造就该等于直连。
        QNetworkProxy proxy{QNetworkProxy::NoProxy};
        QString source;   ///< 「系统代理」「环境变量 HTTPS_PROXY」「本机代理（Clash）」「已关闭代理」…

        bool isDirect() const { return proxy.type() == QNetworkProxy::NoProxy; }
    };

    /// 给这个地址挑一个代理。跟随设置里的模式（auto/direct/manual），
    /// 结果会缓存——端口探测有几百毫秒开销，不能每个请求都重来一遍。
    static Choice forUrl(const QUrl &url);

    /// 清掉缓存并立即重算：设置页改完代理后调它，下一次检查就用新值。
    static Choice reload();

    /// 只作废缓存、不重算。给"重试"用：用户可能是先失败、后开 Clash，
    /// 第二次尝试必须重新探测一遍本机端口，否则会抱着"直连"的旧结论一路失败下去。
    static void invalidate();

    /// 一行可读状态，例如「系统代理 HTTP 127.0.0.1:7890」。给日志与设置页用。
    static QString statusText();

    // ---- 以下为纯函数：临时断言程序可直接调用，不依赖网络与设置 ----

    /// 解析代理地址：支持 "http://127.0.0.1:7890"、"socks5://127.0.0.1:1080"、
    /// 裸 "127.0.0.1:7890"（按 HTTP 处理）、带 "user:pass@" 的写法。
    /// 解析失败返回 NoProxy（调用方据此降级直连，绝不抛异常）。
    static QNetworkProxy parseProxyUrl(const QString &raw);

    /// NO_PROXY 环境变量是否命中该主机（逗号分隔，"*" 表示全部直连）。
    static bool matchesNoProxy(const QString &host, const QString &noProxy);

    /// "HTTP 127.0.0.1:7890" / "SOCKS5 127.0.0.1:1080"。
    static QString describe(const QNetworkProxy &proxy);

    /// 配置里的类型串与枚举互转（ini 里存 "http"/"socks5"，可读、不随枚举变）。
    static QString typeToString(QNetworkProxy::ProxyType type);
    static QNetworkProxy::ProxyType typeFromString(const QString &raw);

private:
    NetProxy() = default;

    /// 真正干活的那次解析（不看缓存）。
    static Choice resolve(const QUrl &url);
    static Choice fromEnvironment(const QUrl &url);
    static Choice fromSystem(const QUrl &url);
    static Choice fromLocalPorts();
};

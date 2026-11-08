#pragma once

// ---------------------------------------------------------------------------
// 局域网共享服务：一个极简的 HTTP/1.1 服务器（只服务本插件定义的那几个路由）。
//
// 为什么自己写 HTTP 而不引第三方库：仓库没有任何网络框架依赖，需求也只是
// 「一个页面 + 几个 JSON 接口 + 文件收发」，Qt 的 QTcpServer 已经够用；
// 引入 civetweb/cpp-httplib 会多一份跨平台构建负担，而三端可用是硬要求。
//
// 为什么是进程级单例：一个端口只能被一个 socket 监听，而宿主允许同一工具同时存在
// 「内嵌面板」与「独立窗口面板」两份。面板只是视图 —— 面板销毁、用户切到别的工具时
// 服务继续跑，正在下载的浏览器不会被打断；重新进入工具能看到同一份列表。
// ---------------------------------------------------------------------------

#include "ShareHttp.h"

#include <QList>
#include <QObject>
#include <QString>

class QTcpServer;

namespace share {

class ShareServer : public QObject
{
    Q_OBJECT

public:
    static ShareServer &instance();

    /// 启动监听。preferredPort 被占用时自动往后顺延（最多 20 个），
    /// 失败时把可读原因写进 error。已在运行且端口相同时直接返回 true。
    bool start(quint16 preferredPort, QString *error);
    void stop();
    bool isRunning() const;
    quint16 port() const { return m_port; }

    /// 所有可访问地址（家用局域网优先）。每次调用都重新枚举网卡，
    /// 因为笔记本换 WiFi / 插拔网线后地址会变。
    QList<LocalAddress> addresses() const;

    /// 首选地址（列表第一个）。没有可用网卡时返回 http://127.0.0.1:port，
    /// 让用户至少能在本机浏览器里验证服务本身是活的。
    QString primaryUrl() const;

    /// 默认端口。5421 只是随手挑的一个不碰常见服务的高位端口，
    /// 与截图里那个工具一致，方便用户按习惯记住。
    static constexpr quint16 kDefaultPort = 5421;

signals:
    /// 开始/停止监听，或监听端口发生变化。
    void statusChanged();

private:
    explicit ShareServer(QObject *parent = nullptr);

    QTcpServer *m_server = nullptr;
    quint16 m_port = 0;
};

} // namespace share

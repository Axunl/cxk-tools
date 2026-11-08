#pragma once

// ---------------------------------------------------------------------------
// 单实例守卫。
//
// 用 QLocalServer（命名管道/Unix socket）实现，而不是 Windows 互斥体或锁文件：
//   * 跨平台一套代码；
//   * 顺带提供「第二个实例通知第一个实例并唤出窗口」的通道 —— 用户重复双击
//     图标时，体验上应该是把已有窗口拉到前面，而不是弹一句"已在运行"。
//
// 依赖 Qt6::Network（QLocalServer 在这个模块里）。
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>
#include <functional>

class QLocalServer;

class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(const QString &key, QObject *parent = nullptr);

    /// 尝试成为主实例。
    /// @return true = 本进程是唯一实例，可以继续启动；
    ///         false = 已有实例，且已通知它唤出窗口，本进程应立即退出。
    bool tryLock();

    /// 收到其他实例的 activate 请求时做什么。必须在 tryLock() 之前或紧随其后设置。
    void setActivateHandler(std::function<void()> handler);

private:
    void onNewConnection();

    QString m_key;
    QLocalServer *m_server = nullptr;
    std::function<void()> m_handler;
};

#pragma once

// ---------------------------------------------------------------------------
// frpc 进程服务：**插件模块级的单例**，不属于任何一个面板。
//
// 为什么不是「面板持有 QProcess」：
//   宿主的约定是「面板不缓存，离开工具视图即销毁」（docs/PLUGIN.md、README「界面与能力」）。
//   frpc 是常驻服务，用户开它就是为了 ssh / 远程桌面能一直连进来；若进程跟着面板一起
//   被带走，用户切个工具穿透就断了 —— 这类工具最不能容忍的行为。screenrec 的录制器
//   是同一个问题、同一套解法：service 是函数内 static，面板只订阅信号、只渲染状态，
//   析构时什么都不做。
//
// 于是本工具能做到：
//   * 启动后切页 / 回首页 / 开独立窗口，frpc 照跑；
//   * 再进来时读到「运行中 00:12:31」与之前的日志；
//   * 退出工具箱时 frpc 跟着停（见 .cpp 里 aboutToQuit 的说明），不会留下无人认领的
//     后台进程 —— 这一点与 detached 方案相反，是刻意选择：一个「工具箱里的小工具」
//     不该在程序退出后继续偷偷跑网络服务。
//
// 同一时间只跑一份配置：frpc 的多实例对普通用户只会带来「端口冲突 / 日志串台」，
// 真有需求就写进同一份配置的多个 [[proxies]]（这是 frp 的推荐用法）。
//
// 日志放在内存里（环形，最多 kMaxBufferedLines 行）：frpc 的输出量不大（登录 + 每条
// 转发的建立/断开），不写磁盘既省事也避免权限问题。代价是退出工具箱后日志不保留，
// 这在「工具」定位下可接受；真要长期留档，用户可以让 frpc 自己写文件（log.to=file），
// 本工具会提示那样面板就看不到日志了。
// ---------------------------------------------------------------------------

#include "FrpcText.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QProcess;
class QTimer;

namespace frpc {

/// 启动 / 停止的结果。失败原因直接展示给用户，不再翻译。
struct Outcome
{
    bool ok = false;
    QString error;
};

/// 配置文件与可执行文件的定位工具（无状态，随时可调）。
namespace paths {

/// AppLocalDataLocation/frpc/conf，存放每次启动前落盘的配置文件。
QString configDir();

/// 配置文件路径。扩展名跟着内容格式走，frpc 靠它挑解析器。
QString configPathFor(const QString &profileId, frpctext::Format format);

/// 可执行文件名（Windows 带 .exe）。
QString executableName();

} // namespace paths

/// 找 frpc 可执行文件：优先用用户填的路径，其次 PATH、程序目录、插件目录、
/// AppLocalData/frpc/bin。找不到返回空串。
QString locate(const QString &preferred = QString());

/// 找不到 frpc 时给用户的一句完整建议（含从哪里下载）。
QString locateHint();

///
/// frpc 运行服务。通过 instance() 取单例，不要自己 new。
///
class Service : public QObject
{
    Q_OBJECT

public:
    enum class Phase
    {
        Idle,      ///< 没在跑
        Running,   ///< 进程活着
        Stopping   ///< 已发停止信号，等进程退出（POSIX 上 SIGTERM 可能被忽略）
    };
    Q_ENUM(Phase)

    struct Status
    {
        Phase phase = Phase::Idle;
        QString profileId;      ///< 当前 / 上次运行的是哪份配置
        QString program;        ///< 实际使用的可执行文件
        qint64 pid = 0;
        qint64 startedAtMs = 0; ///< 启动时刻（毫秒时间戳），面板用来算运行时长
        QString lastError;      ///< 启动失败 / 异常退出的原因（面板原样显示）
        int lastExitCode = 0;
        bool lastCrashed = false;   ///< 上次退出是不是崩溃（非正常退出）
    };

    static Service *instance();

    const Status &status() const { return m_status; }
    bool isRunning() const { return m_status.phase == Phase::Running; }
    bool isStopping() const { return m_status.phase == Phase::Stopping; }

    /// 当前运行的是不是这份配置。面板据此决定按钮是「启动」还是「停止」。
    bool runsProfile(const QString &profileId) const;

    /// 启动。会把配置原子写到 conf/<id>.<ext>，然后 `frpc -c <file>`。
    /// 已经在跑时返回失败（不叠第二个实例）。
    Outcome start(const QString &profileId, const QString &program, const QString &configText,
                  frpctext::Format format);

    /// 停止：POSIX 先 SIGTERM，4 秒不退再 SIGKILL；Windows 直接 TerminateProcess
    /// （frpc 是控制台程序，没有窗口消息循环可以收 WM_CLOSE，terminate() 等于没发）。
    /// 不阻塞等待，结果通过 stateChanged() 通知。
    Outcome stop();

    /// 取日志增量。
    ///
    /// @param index 上次返回的 next（首次传 0）
    /// @return lines 为新行；next 为下次要传的 index；lostEarlier 表示 index 太旧、
    ///         缓冲区已经把它滚掉了，面板应丢弃已显示的旧行（否则会与现有日志错位）。
    struct LogChunk
    {
        QStringList lines;
        qint64 next = 0;
        bool lostEarlier = false;
    };
    LogChunk since(qint64 index) const;

    /// 最近 maxLines 行（面板重新创建时恢复现场用）。
    QStringList tail(int maxLines) const;

    /// 已产生的总行数（绝对计数，不随缓冲区滚动变化）。
    qint64 lineCount() const { return m_totalLines; }

    /// 清空缓冲区。运行中的 frpc 不受影响，之后的新输出照常进来。
    void clearLog();

signals:
    /// 有新日志行（面板可以立刻拉取，不必等定时器）
    void outputArrived();

    /// 运行状态变了（启动成功、退出、停止中）
    void stateChanged();

private:
    explicit Service();
    ~Service() override;

    /// 把一段原始输出按行切开，处理「一次读到半行」的情况
    void consume(const QByteArray &raw);

    /// 追加一行到环形缓冲区（超过上限从头丢）
    void appendLine(const QString &line);

    /// 进程结束后的收尾：记录退出码、清状态、发信号
    void handleFinished(int exitCode, int exitStatus);

    QProcess *m_process = nullptr;
    QTimer *m_forceKillTimer = nullptr;
    Status m_status;

    QStringList m_lines;      ///< 最近日志（最多 kMaxBufferedLines 行）
    qint64 m_totalLines = 0;  ///< 历史总行数（绝对计数）
    QByteArray m_partial;     ///< 还没读到换行的半行
};

} // namespace frpc

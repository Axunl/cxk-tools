#pragma once

// ---------------------------------------------------------------------------
// 进程探针：本插件里唯一碰平台 API 的地方。
//
// 只对外暴露 Qt 值类型（quint32 / QString / quint64），平台头文件全部关在
// ProcProbe.cpp 里。这样 ProcText.h 的纯逻辑（单位换算、筛选排序、占用率计算）
// 可以在临时工程里直接 include 做断言，不需要拉起 Win32 或 procfs。
//
// 平台能力差异（改代码前先看这张表）：
//   * 系统内存、进程内存、进程 CPU 时间：三端都实现了；
//   * 整机网络累计流量：三端都实现了（面板自己按两次采样做差算速率）；
//   * 监听端口：三端都实现了（Win: GetExtendedTcpTable / Linux: /proc/net + fd
//     反查 / macOS: lsof）；
//   * 进程磁盘 I/O：Windows、Linux 有；**macOS 没有**——面板上那一列显示 "—"，
//     原因见 unsupportedDiskReason()；
//   * 结束进程：三端都用原生方式（TerminateProcess / kill）；
//   * 重启进程：先读命令行与工作目录，读不到就如实报错（不"猜参数"）；
//   * 回收内存：**只有 Windows 能对别的进程做**（EmptyWorkingSet）。
//     Linux 内核没有普通用户可用的"刷出某进程工作集"接口，macOS 同样没有公开
//     API（`purge` 要 root 且动的是系统缓存而不是进程工作集）。这两个平台
//     如实返回失败原因，不假装成功、也不偷偷降级成"清缓存"。
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace procprobe {

/// 单个进程的一次采样快照（值类型，不是句柄）。
///
/// CPU 与磁盘给的是**累计值**而不是百分比/速率：占用率必须靠两次采样做差，
/// 探针保持无状态，差值计算放在 ProcText.h 里（纯函数，可单独断言）。
struct ProcessInfo
{
    quint32 pid = 0;
    QString name;              ///< 进程名（Win: szExeFile，Linux: comm，macOS: comm 末段）
    QString exePath;           ///< 可执行文件完整路径；受保护进程读不到时为空
    quint64 workingSet = 0;    ///< 物理内存（工作集 / RSS），字节。memoryReadable 为 false 时无意义
    quint64 cpuTimeUs = 0;     ///< 累计 CPU 时间（用户 + 内核），微秒
    quint64 diskReadBytes = 0; ///< 累计读盘字节（仅 Windows / Linux 有值）
    quint64 diskWriteBytes = 0;///< 累计写盘字节（仅 Windows / Linux 有值）
    QVector<quint16> ports;    ///< 监听端口（TCP LISTEN + 已绑定的 UDP），去重升序
    bool memoryReadable = false;    ///< 内存数字是否可信（受保护的系统进程读不到）
    bool countersReadable = false;  ///< CPU / 磁盘累计值是否可信
};

/// 系统物理内存快照。
struct SystemMemory
{
    quint64 totalBytes = 0;
    quint64 availBytes = 0;   ///< "可用"：Windows 用 ullAvailPhys，Linux 用 MemAvailable，macOS 用 free+inactive
    bool valid = false;       ///< false 表示本平台读不到（面板会显示占位文案，不会崩）
};

/// 整机网络累计流量（所有非回环网卡相加）。面板按两次采样做差算速率。
struct SystemNet
{
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
    bool valid = false;
};

/// 一次内存回收操作的结果。
///
/// freedBytes() 是"工作集减少量"，不等于"可用内存增加量"：被刷出的页可能马上又被
/// 访问换回来，也可能直接进了待机列表。给用户看的是这个减少量，符合直觉就够了。
struct TrimOutcome
{
    bool ok = false;
    quint64 beforeBytes = 0;
    quint64 afterBytes = 0;
    QString error;            ///< ok == false 时的可读原因（直接显示给用户）

    quint64 freedBytes() const
    {
        return beforeBytes > afterBytes ? beforeBytes - afterBytes : 0;
    }
};

/// 结束进程的结果。
struct KillOutcome
{
    bool ok = false;
    QString error;            ///< ok == false 时的可读原因（直接显示给用户）
};

/// 重启进程所需的信息：程序 + 参数 + 工作目录。
///
/// 三者缺一不可：只拿程序路径重启，带参数启动的进程（node 脚本、IDE、带文件
/// 打开的应用）会变成"启动了一个空壳"，比不重启更让人困惑。所以读不到命令行
/// 就报错，不去猜。
struct ProcessCommand
{
    QString program;
    QStringList args;
    QString workingDir;       ///< 空表示沿用当前工作目录
    bool valid = false;
    QString error;
};

/// 读系统物理内存总量与可用量。失败时返回 valid == false。
SystemMemory readSystemMemory();

/// 读整机网络累计收/发字节（回环网卡不计）。
SystemNet readSystemNet();

/// 列出当前所有进程及其内存 / CPU / 磁盘 / 端口快照。
///
/// 读不到内存的进程（受保护的系统进程）也会返回，只是 memoryReadable == false ——
/// 直接从列表里抹掉会让用户以为"系统进程不存在"，留着显示"—"更诚实。
QVector<ProcessInfo> listProcesses();

/// 读某个进程的启动命令行与工作目录，供"重启"用。
ProcessCommand readCommand(quint32 pid);

/// 结束进程（不弹确认，确认由面板负责）。返回前会等进程真的退出：
/// 调用方紧接着可能要重启同一个程序，进程没退干净会撞端口/文件锁。
KillOutcome killProcess(quint32 pid);

/// 在文件管理器里打开 exePath 所在目录并选中它。
/// 失败时把原因写进 error（可能为 nullptr）。
bool revealProcessFile(const QString &exePath, QString *error);

/// 把某个进程的工作集刷出物理内存（写入页面文件 / 待机列表）。
///
/// 取的是 EmptyWorkingSet 语义：进程不退出、数据不丢，只是短期要重新换页，
/// 因此**会变慢**。调用方不要对同一个进程高频重复调用。
TrimOutcome trimProcess(quint32 pid);

/// 当前平台能否回收**其他进程**的内存。false 时 trimProcess() 必定失败，
/// 面板据此把按钮置灰并给出原因。
bool canTrimOtherProcesses();

/// canTrimOtherProcesses() 为 false 时的可读原因（也是 trimProcess 的 error）。
QString unsupportedTrimReason();

/// 当前平台能否按进程统计磁盘 I/O。false 时那一列显示 "—"。
bool canReadDiskCounters();

/// canReadDiskCounters() 为 false 时的可读原因（悬浮提示用）。
QString unsupportedDiskReason();

} // namespace procprobe

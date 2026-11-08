#pragma once

// ---------------------------------------------------------------------------
// 进程 / 内存探针：本插件里唯一碰平台 API 的地方。
//
// 只对外暴露 Qt 值类型（quint32 / QString / quint64），平台头文件全部关在
// MemProbe.cpp 里。这样 MemText.h 的纯逻辑（单位换算、排序、候选筛选）可以在
// 临时工程里直接 include 做断言，不需要拉起 Win32 或 procfs。
//
// 平台能力差异（改代码前先看这张表）：
//   * 读系统内存、列进程内存：Windows / Linux / macOS 三端都实现了；
//   * 回收内存：**只有 Windows 能对别的进程做**（EmptyWorkingSet）。
//     Linux 内核没有普通用户可用的"刷出某进程工作集"接口，macOS 同样没有公开
//     API（`purge` 要 root 且动的是系统缓存而不是进程工作集）。这两个平台
//     如实返回失败原因，不假装成功、也不偷偷降级成"清缓存"。
// ---------------------------------------------------------------------------

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace memprobe {

/// 单个进程的内存快照（一次采样的结果，不是句柄）。
struct ProcessMemory
{
    quint32 pid = 0;
    QString name;             ///< 可执行文件名（Windows 取 szExeFile，Linux 取 comm，macOS 取 comm 的末段）
    quint64 workingSet = 0;   ///< 物理内存工作集，字节。readable 为 false 时无意义
    bool readable = false;    ///< 是否读到了内存信息（受保护的系统进程读不到）
};

/// 系统物理内存快照。
struct SystemMemory
{
    quint64 totalBytes = 0;
    quint64 availBytes = 0;   ///< "可用"：Windows 用 ullAvailPhys，Linux 用 MemAvailable，macOS 用 free+inactive
    bool valid = false;       ///< false 表示本平台读不到（面板会显示占位文案，不会崩）
};

/// 一次回收操作的结果。
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

/// 读系统物理内存总量与可用量。失败时返回 valid == false。
SystemMemory readSystemMemory();

/// 列出当前所有进程及其工作集。
///
/// 读不到内存的进程（受保护的系统进程）也会返回，只是 readable == false ——
/// 直接从列表里抹掉会让用户以为"系统进程不存在"，留着显示"—"更诚实。
QVector<ProcessMemory> listProcesses();

/// 把某个进程的工作集刷出物理内存（写入页面文件 / 待机列表）。
///
/// 取的是 EmptyWorkingSet 语义：进程不退出、数据不丢，只是短期内要重新换页，
/// 因此**会变慢**。调用方不要对同一个进程高频重复调用。
TrimOutcome trimProcess(quint32 pid);

/// 当前平台能否回收**其他进程**的内存。false 时 trimProcess() 必定失败，
/// 面板据此把按钮置灰并给出原因。
bool canTrimOtherProcesses();

/// canTrimOtherProcesses() 为 false 时的可读原因（也是 trimProcess 的 error）。
QString unsupportedTrimReason();

} // namespace memprobe

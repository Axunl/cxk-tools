#pragma once

// ---------------------------------------------------------------------------
// 文件占用释放：平台探针。本插件里唯一碰原生 API 的地方。
//
// 对外只暴露 FileLockLogic.h 里的 Qt 值类型结构体，平台头文件全部关在 .cpp 里，
// 这样纯逻辑与面板都能在临时工程里单独编译。
//
// 平台能力表（改代码前先看这张，UI 的按钮可用性全部按它来）：
//   * 检测占用：Windows 用 Restart Manager（官方接口，按文件精确查）；
//     Linux 扫 /proc/<pid>/fd 的符号链接；macOS 调系统自带 lsof。
//   * 解除占用（关闭别人进程里的文件句柄）：**只有 Windows 做得到**。
//     Linux / macOS 没有普通用户可用的「替别的进程关 fd」接口，如实返回原因，
//     不偷偷降级成「杀进程」——那是「强制结束」按钮的语义。
//   * 强制结束：Windows TerminateProcess、Linux/macOS SIGTERM，三端都有。
// ---------------------------------------------------------------------------

#include "FileLockLogic.h"

#include <QString>
#include <QVector>

namespace fileprobe {

/// 当前平台能否检测占用。兜底平台返回 false，面板会禁用检测并给出原因。
bool canDetect();

/// 当前平台能否「解除占用」（关闭其他进程持有的文件句柄）。
bool canReleaseHandles();

/// 当前平台能否强制结束进程。
bool canTerminate();

/// 上述能力为 false 时给用户的可读原因（按钮 tooltip 与状态栏共用）。
QString unsupportedDetectReason();
QString unsupportedReleaseReason();
QString unsupportedTerminateReason();

/// 检测占用 inputs（文件 / 文件夹混合）的进程，结果已按 pid 归并。
///
/// 可能耗时（Linux 要扫全部进程的 fd、macOS 要起 lsof），调用方应放到工作线程；
/// 本函数不访问任何 QWidget，也不要求运行在 UI 线程。
/// selfPid 用于排除宿主自己 —— 让用户把自己结束掉是灾难性的。
filelock::ScanOutcome scan(const QStringList &inputs, bool recursive, quint64 selfPid);

/// 关闭 pids 这些进程里指向目标文件的句柄。平台不支持时 outcome.error 非空。
filelock::ActionOutcome release(const QStringList &inputs, bool recursive,
                                const QVector<quint64> &pids, quint64 selfPid);

/// 强制结束 pids（Windows TerminateProcess / POSIX SIGTERM）。
/// 调用方必须先做二次确认：被结束的进程里可能有未保存的数据。
filelock::ActionOutcome terminate(const QVector<quint64> &pids, quint64 selfPid);

} // namespace fileprobe

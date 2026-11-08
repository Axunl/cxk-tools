#pragma once

// ---------------------------------------------------------------------------
// 内存监控的纯逻辑：单位换算、占用率、筛选排序、回收候选。
//
// 抽成 header 的原因：这些规则（多少算"内存吃紧"、哪些进程值得回收）最容易写错，
// 也最适合单测。临时工程只要 include 这个头就能跑断言，不必构造界面、也不必
// 真的去刷别人的工作集。
//
// 全部 inline：这个头会被多个 TU 包含，不 inline 会撞重复定义。
// ---------------------------------------------------------------------------

#include "MemProbe.h"

#include <QDateTime>
#include <QString>
#include <QVector>

#include <algorithm>

namespace memtext {

/// 容量的可读写法，1024 进制（与任务管理器/活动监视器的口径一致）。
///
/// 分级规则是刻意的：GB 保留 1 位小数（32.0 GB）；100 MB 以上取整（512 MB，
/// 小数位没有意义）；100 MB 以下保留 1 位（23.4 MB，避免"23 MB"和"24 MB"
/// 挤成一团）；1 KB 以下直接给字节数。
inline QString formatBytes(quint64 bytes)
{
    constexpr double kKb = 1024.0;
    constexpr double kMb = 1024.0 * 1024.0;
    constexpr double kGb = 1024.0 * 1024.0 * 1024.0;

    if (bytes >= kGb)
        return QStringLiteral("%1 GB").arg(bytes / kGb, 0, 'f', 1);
    if (bytes >= 100.0 * kMb)
        return QStringLiteral("%1 MB").arg(bytes / kMb, 0, 'f', 0);
    if (bytes >= kMb)
        return QStringLiteral("%1 MB").arg(bytes / kMb, 0, 'f', 1);
    if (bytes >= kKb)
        return QStringLiteral("%1 KB").arg(bytes / kKb, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

/// 百分比文案。decimals 为 0 时是 "68%"（概览大字用）。
inline QString formatPercent(double percent, int decimals = 0)
{
    return QStringLiteral("%1%").arg(percent, 0, 'f', decimals);
}

/// 已用百分比（0~100）。total 为 0 时返回 0，避免除零。
inline double usedPercent(const memprobe::SystemMemory &mem)
{
    if (!mem.valid || mem.totalBytes == 0)
        return 0.0;
    const quint64 avail = qMin(mem.availBytes, mem.totalBytes);
    return 100.0 * static_cast<double>(mem.totalBytes - avail) / static_cast<double>(mem.totalBytes);
}

/// 可用百分比（0~100）。
inline double availablePercent(const memprobe::SystemMemory &mem)
{
    if (!mem.valid || mem.totalBytes == 0)
        return 0.0;
    const quint64 avail = qMin(mem.availBytes, mem.totalBytes);
    return 100.0 * static_cast<double>(avail) / static_cast<double>(mem.totalBytes);
}

/// 可用内存是否已跌破阈值（thresholdPercent 指"可用占总量的百分比"）。
///
/// 用"可用低于 X%"而不是"已用高于 X%"：两者互补但后者在文案上容易搞混
/// （"已用 85%"到底是警戒线还是正常？），而"只剩 15% 可用"没有歧义。
inline bool isLowMemory(const memprobe::SystemMemory &mem, int thresholdPercent)
{
    if (!mem.valid || mem.totalBytes == 0)
        return false;
    return availablePercent(mem) < static_cast<double>(thresholdPercent);
}

/// 概览卡的警戒等级：0 正常、1 偏高、2 紧张。
///
/// 阈值取 80/90 而不是 70/85：现代系统光是文件缓存就能把"已用"顶到 70% 上下，
/// 低于 80% 就变橙会变成天天报警，用户很快就会无视颜色。
inline int levelFor(double used)
{
    if (used >= 90.0)
        return 2;
    if (used >= 80.0)
        return 1;
    return 0;
}

/// 自动释放的决策：可用内存跌破阈值，且距上次自动释放已过冷却时间。
///
/// 抽成纯函数的原因：冷却最容易写错（首次为空的判定、秒/毫秒混用），
/// 而这条路径要靠"机器真的内存不足"才跑得到 —— 放到纯逻辑里才能稳定断言。
inline bool shouldAutoRelease(const memprobe::SystemMemory &mem, int thresholdPercent,
                              const QDateTime &lastRelease, const QDateTime &now,
                              int cooldownSecs)
{
    if (!isLowMemory(mem, thresholdPercent))
        return false;
    if (!lastRelease.isValid())
        return true;   // 还没自动释放过
    return lastRelease.secsTo(now) >= cooldownSecs;
}

/// 按关键词筛选并按"内存从大到小"排序。
///
/// 匹配规则：全数字关键词按 PID 前缀匹配（输入 "18" 能命中 1823），否则按进程名
/// 不区分大小写的子串匹配（中文输入法切错时也不会空手而归）。
/// 排序规则：能读到的在前按工作集降序；读不到的沉底按名字排 —— 否则那一堆
/// 显示"—"的系统进程会夹在中间，看起来像排序坏了。
inline QVector<memprobe::ProcessMemory> filterAndSort(
    const QVector<memprobe::ProcessMemory> &all, const QString &query)
{
    const QString key = query.trimmed();
    const bool pidQuery = !key.isEmpty()
        && std::all_of(key.cbegin(), key.cend(),
                       [](QChar ch) { return ch.isDigit(); });

    QVector<memprobe::ProcessMemory> out;
    out.reserve(all.size());
    for (const memprobe::ProcessMemory &p : all) {
        if (key.isEmpty()) {
            out.append(p);
            continue;
        }
        if (pidQuery) {
            if (QString::number(p.pid).contains(key))
                out.append(p);
        } else if (p.name.contains(key, Qt::CaseInsensitive)) {
            out.append(p);
        }
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const memprobe::ProcessMemory &a, const memprobe::ProcessMemory &b) {
                         if (a.readable != b.readable)
                             return a.readable;   // 能读到的永远在前
                         if (!a.readable)
                             return a.name.localeAwareCompare(b.name) < 0;
                         if (a.workingSet != b.workingSet)
                             return a.workingSet > b.workingSet;
                         return a.name.localeAwareCompare(b.name) < 0;
                     });
    return out;
}

/// 挑出值得回收的进程，按工作集从大到小，最多 maxCount 个。
///
/// 排除项：excludePid（当前进程自己，刷自己没意义还会让界面重新分配）、
/// pid 0/4（系统空闲/System，打不开）、读不到的、小于 minBytes 的。
/// minBytes 的作用是防止把几百个小进程全刷一遍 —— 那样收益极低，副作用却摊到
/// 整个系统上。
inline QVector<quint32> pickReleaseCandidates(const QVector<memprobe::ProcessMemory> &all,
                                              quint64 minBytes, int maxCount, quint32 excludePid)
{
    QVector<memprobe::ProcessMemory> sorted = all;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const memprobe::ProcessMemory &a, const memprobe::ProcessMemory &b) {
                         return a.workingSet > b.workingSet;
                     });

    QVector<quint32> pids;
    for (const memprobe::ProcessMemory &p : sorted) {
        if (pids.size() >= maxCount)
            break;
        if (!p.readable || p.workingSet < minBytes)
            continue;
        if (p.pid == excludePid || p.pid == 0 || p.pid == 4)
            continue;
        pids.append(p.pid);
    }
    return pids;
}

} // namespace memtext

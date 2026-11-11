#pragma once

// ---------------------------------------------------------------------------
// 进程监控的纯逻辑：单位换算、占用率、采样差值、筛选排序、回收候选。
//
// 抽成 header 的原因：这些规则（多少算"内存吃紧"、哪个进程值得回收、CPU 占用
// 怎么由累计时间算出来）最容易写错，也最适合单测。临时工程只要 include 这个头
// 就能跑断言，不必构造界面、也不必真的去刷别人的工作集。
//
// 全部 inline：这个头会被多个 TU 包含，不 inline 会撞重复定义。
// ---------------------------------------------------------------------------

#include "ProcProbe.h"

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

#include <algorithm>

namespace proctext {

/// 表格的排序列。端口列按"第一个端口"排，没有端口的沉底。
enum class SortKey
{
    Name,
    Pid,
    Cpu,
    Memory,
    Disk,
    Port
};

/// 两次采样之间算出来的速率。
struct ProcRates
{
    double cpuPercent = 0.0;        ///< 占整机 CPU 的百分比（0~100×核数之间的值，见下）
    double diskBytesPerSec = 0.0;   ///< 读 + 写合计速率
    bool valid = false;             ///< false 表示这次算不出来（新出现的进程 / 计数器读不到）
};

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

/// 速率的可读写法：formatBytes 加 "/s"。
inline QString formatRate(double bytesPerSec)
{
    if (bytesPerSec <= 0.0)
        return QStringLiteral("0 B/s");
    // 小于 1 字节/秒的抖动直接算 0，别显示 "0 B/s" 之外的怪数字
    return formatBytes(static_cast<quint64>(bytesPerSec)) + QStringLiteral("/s");
}

/// 百分比文案。decimals 为 0 时是 "68%"（概览大字用）。
inline QString formatPercent(double percent, int decimals = 0)
{
    return QStringLiteral("%1%").arg(percent, 0, 'f', decimals);
}

/// CPU 占用文案。0 就写 0%，很小的占用写 "<0.1%"，
/// 否则保留 1 位 —— 否则一堆进程都是 "0.0%"，看不出谁在动。
inline QString formatCpu(double percent)
{
    if (percent <= 0.0)
        return QStringLiteral("0%");
    if (percent < 0.1)
        return QStringLiteral("<0.1%");
    return QStringLiteral("%1%").arg(percent, 0, 'f', 1);
}

/// 端口列表文案：最多显示 maxShown 个，其余折成 " +N"。
/// 端口多的进程（数据库、IDE）会把这一列撑开，所以必须截断。
inline QString formatPorts(const QVector<quint16> &ports, int maxShown = 3)
{
    if (ports.isEmpty())
        return QStringLiteral("—");
    QStringList shown;
    const int count = qMin(maxShown, static_cast<int>(ports.size()));
    for (int i = 0; i < count; ++i)
        shown.append(QString::number(ports.at(i)));
    QString text = shown.join(QStringLiteral(", "));
    if (ports.size() > count)
        text += QStringLiteral(" +%1").arg(ports.size() - count);
    return text;
}

/// 已用百分比（0~100）。total 为 0 时返回 0，避免除零。
inline double usedPercent(const procprobe::SystemMemory &mem)
{
    if (!mem.valid || mem.totalBytes == 0)
        return 0.0;
    const quint64 avail = qMin(mem.availBytes, mem.totalBytes);
    return 100.0 * static_cast<double>(mem.totalBytes - avail) / static_cast<double>(mem.totalBytes);
}

/// 可用百分比（0~100）。
inline double availablePercent(const procprobe::SystemMemory &mem)
{
    if (!mem.valid || mem.totalBytes == 0)
        return 0.0;
    const quint64 avail = qMin(mem.availBytes, mem.totalBytes);
    return 100.0 * static_cast<double>(avail) / static_cast<double>(mem.totalBytes);
}

/// "已用 ≥ usedThreshold%" 转成内部用的"可用 < 剩余%"。
///
/// 界面上给用户看的是"占用到 80% 就自动释放"（这是任务管理器式的说法），
/// 但判定函数吃的是可用阈值 —— 两者互补，这里做一次换算，别让调用方各写一遍。
inline int availableThreshold(int usedThresholdPercent)
{
    return qBound(0, 100 - usedThresholdPercent, 100);
}

/// 可用内存是否已跌破阈值（thresholdPercent 指"可用占总量的百分比"）。
inline bool isLowMemory(const procprobe::SystemMemory &mem, int thresholdPercent)
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
inline bool shouldAutoRelease(const procprobe::SystemMemory &mem, int thresholdPercent,
                              const QDateTime &lastRelease, const QDateTime &now,
                              int cooldownSecs)
{
    if (!isLowMemory(mem, thresholdPercent))
        return false;
    if (!lastRelease.isValid())
        return true;   // 还没自动释放过
    return lastRelease.secsTo(now) >= cooldownSecs;
}

/// 由两次采样算出每个进程的 CPU 占用与磁盘速率。
///
/// CPU 口径：占**整机**的百分比（与任务管理器一致），所以要把"本进程攒了多少
/// CPU 时间"除以"墙钟时间 × 逻辑核数"。单个进程吃满一个核时显示 100/核数，
/// 比如 8 核机器上是 12.5% —— 这符合用户对"这台机器被占了几个点"的直觉。
///
/// 上次采样里没有的 pid（刚启动）不算速率：没有基准，算出来的值只会是 0 或噪音。
/// 计数器变小的情形（进程重启了计数器、macOS 的 32 位网卡计数器回绕）一律按 0 算，
/// 不能让差值回绕成一个天文数字。
inline QHash<quint32, ProcRates> computeRates(const QVector<procprobe::ProcessInfo> &previous,
                                              const QVector<procprobe::ProcessInfo> &current,
                                              qint64 elapsedMs, int coreCount)
{
    QHash<quint32, ProcRates> out;
    if (elapsedMs < 200 || coreCount <= 0)
        return out;   // 间隔太短时差值全是噪声（比如释放内存后的那次立即刷新）

    QHash<quint32, const procprobe::ProcessInfo *> before;
    before.reserve(previous.size());
    for (const procprobe::ProcessInfo &process : previous)
        before.insert(process.pid, &process);

    const double elapsedSec = static_cast<double>(elapsedMs) / 1000.0;
    out.reserve(current.size());
    for (const procprobe::ProcessInfo &process : current) {
        const auto it = before.constFind(process.pid);
        if (it == before.constEnd() || !process.countersReadable)
            continue;
        const procprobe::ProcessInfo &prev = *it.value();

        ProcRates rates;
        // 微秒 → 秒；再除以核数得到"占整机"的百分比
        const quint64 cpuDelta = process.cpuTimeUs > prev.cpuTimeUs
            ? process.cpuTimeUs - prev.cpuTimeUs
            : 0;
        rates.cpuPercent = 100.0 * (static_cast<double>(cpuDelta) / 1e6) / elapsedSec
            / static_cast<double>(coreCount);

        const quint64 prevDisk = prev.diskReadBytes + prev.diskWriteBytes;
        const quint64 nowDisk = process.diskReadBytes + process.diskWriteBytes;
        const quint64 diskDelta = nowDisk > prevDisk ? nowDisk - prevDisk : 0;
        rates.diskBytesPerSec = static_cast<double>(diskDelta) / elapsedSec;

        rates.valid = true;
        out.insert(process.pid, rates);
    }
    return out;
}

/// 按关键词筛选，再按 sortKey 排序。
///
/// 匹配规则（对应"按端口 / 进程名搜索"这两个入口）：
///   * 全数字关键词同时匹配 PID 与端口 —— 输 "8080" 命中占用该端口的进程，
///     输 "1823" 命中 pid 为 1823 的进程。端口用前缀匹配，避免输 "8" 时把
///     8000/8080/8443 全捞出来；
///   * 其他关键词按进程名或完整路径子串匹配，不区分大小写（中文输入法切错时
///     也不会空手而归）。
///
/// 排序规则：选中的列决定主序；内存 / CPU / 磁盘这三个指标列把读不到数值的
/// 进程沉底 —— 否则那一堆显示"—"的系统进程会夹在中间，看起来像排序坏了。
inline QVector<procprobe::ProcessInfo> filterAndSort(const QVector<procprobe::ProcessInfo> &all,
                                                     const QString &query,
                                                     const QHash<quint32, ProcRates> &rates,
                                                     SortKey sortKey, bool descending)
{
    const QString key = query.trimmed();
    const bool numericQuery = !key.isEmpty()
        && std::all_of(key.cbegin(), key.cend(),
                       [](QChar ch) { return ch.isDigit(); });

    QVector<procprobe::ProcessInfo> out;
    out.reserve(all.size());
    for (const procprobe::ProcessInfo &process : all) {
        if (key.isEmpty()) {
            out.append(process);
            continue;
        }
        if (numericQuery) {
            if (QString::number(process.pid).contains(key))
                out.append(process);
            else {
                const bool portHit = std::any_of(
                    process.ports.cbegin(), process.ports.cend(), [&key](quint16 port) {
                        return QString::number(port).startsWith(key);
                    });
                if (portHit)
                    out.append(process);
            }
        } else if (process.name.contains(key, Qt::CaseInsensitive)
                   || process.exePath.contains(key, Qt::CaseInsensitive)) {
            out.append(process);
        }
    }

    const auto rateOf = [&rates](quint32 pid) {
        const auto it = rates.constFind(pid);
        return it == rates.constEnd() ? ProcRates{} : it.value();
    };

    // 指标列：读不到的沉底（无论升序还是降序）
    const auto metricMissing = [sortKey](const procprobe::ProcessInfo &process) {
        if (sortKey == SortKey::Memory)
            return !process.memoryReadable;
        return !process.countersReadable;
    };

    std::stable_sort(out.begin(), out.end(),
                     [&](const procprobe::ProcessInfo &a, const procprobe::ProcessInfo &b) {
                         const bool metricSort = sortKey == SortKey::Cpu
                             || sortKey == SortKey::Memory || sortKey == SortKey::Disk;
                         if (metricSort && metricMissing(a) != metricMissing(b))
                             return metricMissing(b);   // 有数值的在前

                         int cmp = 0;
                         switch (sortKey) {
                         case SortKey::Name:
                             cmp = a.name.localeAwareCompare(b.name);
                             break;
                         case SortKey::Pid:
                             cmp = a.pid < b.pid ? -1 : (a.pid > b.pid ? 1 : 0);
                             break;
                         case SortKey::Cpu: {
                             const double la = rateOf(a.pid).cpuPercent;
                             const double lb = rateOf(b.pid).cpuPercent;
                             cmp = la < lb ? -1 : (la > lb ? 1 : 0);
                             break;
                         }
                         case SortKey::Memory:
                             cmp = a.workingSet < b.workingSet
                                 ? -1
                                 : (a.workingSet > b.workingSet ? 1 : 0);
                             break;
                         case SortKey::Disk: {
                             const double la = rateOf(a.pid).diskBytesPerSec;
                             const double lb = rateOf(b.pid).diskBytesPerSec;
                             cmp = la < lb ? -1 : (la > lb ? 1 : 0);
                             break;
                         }
                         case SortKey::Port: {
                             const quint16 pa = a.ports.isEmpty() ? 0 : a.ports.first();
                             const quint16 pb = b.ports.isEmpty() ? 0 : b.ports.first();
                             if ((pa == 0) != (pb == 0))
                                 return pb == 0;   // 有端口的在前
                             cmp = pa < pb ? -1 : (pa > pb ? 1 : 0);
                             break;
                         }
                         }
                         if (descending)
                             cmp = -cmp;
                         if (cmp != 0)
                             return cmp < 0;
                         // 完全相等时按名字定序，保证列表不会每 2 秒跳一下
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
inline QVector<quint32> pickReleaseCandidates(const QVector<procprobe::ProcessInfo> &all,
                                              quint64 minBytes, int maxCount, quint32 excludePid)
{
    QVector<procprobe::ProcessInfo> sorted = all;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const procprobe::ProcessInfo &a, const procprobe::ProcessInfo &b) {
                         return a.workingSet > b.workingSet;
                     });

    QVector<quint32> pids;
    for (const procprobe::ProcessInfo &process : sorted) {
        if (pids.size() >= maxCount)
            break;
        if (!process.memoryReadable || process.workingSet < minBytes)
            continue;
        if (process.pid == excludePid || process.pid == 0 || process.pid == 4)
            continue;
        pids.append(process.pid);
    }
    return pids;
}

} // namespace proctext

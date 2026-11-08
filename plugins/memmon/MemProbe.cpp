#include "MemProbe.h"

// ---------------------------------------------------------------------------
// 平台实现。三段结构固定：Windows / Linux / macOS，其余平台走兜底分支。
//
// 注意：本文件是插件里唯一允许出现原生头文件的地方。往这里加东西时别忘了
// 其他两段也要有对应实现或明确的不支持文案。
// ---------------------------------------------------------------------------

#include <QDir>
#include <QFile>
#include <QList>
#include <QProcess>
#include <QStringList>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <psapi.h>
#  include <tlhelp32.h>   // CreateToolhelp32Snapshot：windows.h 不带，要单独引
#elif defined(Q_OS_MACOS)
#  include <mach/mach.h>
#  include <sys/sysctl.h>
#  include <unistd.h>
#elif defined(Q_OS_LINUX)
#  include <unistd.h>   // sysconf(_SC_PAGESIZE)
#endif

namespace memprobe {

namespace {

#if defined(Q_OS_LINUX)

/// 从 /proc/meminfo 的一行里取数值（单位 kB）。
/// 行格式固定为 "MemTotal:       16316344 kB"，取第二个字段即可。
quint64 parseKbLine(const QByteArray &line)
{
    const QList<QByteArray> parts = line.simplified().split(' ');
    if (parts.size() < 2)
        return 0;
    bool ok = false;
    const quint64 kb = parts.at(1).toULongLong(&ok);
    return ok ? kb : 0;
}

/// 读 /proc/<pid>/statm 的第一个字段（size）与第二个字段（resident），单位是页。
/// statm 比 status 便宜：一行整数，不用解析键值对。
bool readStatm(const QString &pidDir, quint64 pageSize, quint64 *workingSet)
{
    QFile statm(pidDir + QStringLiteral("/statm"));
    if (!statm.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
    if (fields.size() < 2)
        return false;
    bool ok = false;
    const quint64 residentPages = fields.at(1).toULongLong(&ok);
    if (!ok)
        return false;
    *workingSet = residentPages * pageSize;
    return true;
}

/// 进程名走 /proc/<pid>/comm（单行，最长 16 字节），读不到就退回 pid 目录名。
QString readComm(const QString &pidDir, quint32 pid)
{
    QFile comm(pidDir + QStringLiteral("/comm"));
    if (!comm.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("(pid %1)").arg(pid);
    return QString::fromUtf8(comm.readAll()).trimmed();
}

#endif // Q_OS_LINUX

#if defined(Q_OS_MACOS)

/// 用 `ps` 列进程。
///
/// 为什么不用 sysctl(KERN_PROC_ALL)：那套结构体在不同 macOS 版本上字段偏移变过，
/// 且要自己算 kinfo_proc 大小；`ps -axo pid=,rss=,comm=` 的输出格式二十年没变，
/// 解析成本远低于维护一段脆弱的 ABI 代码。代价是一次进程外调用（约 10ms），
/// 面板 2 秒刷一次完全够用。
QVector<ProcessMemory> listProcessesViaPs()
{
    QVector<ProcessMemory> out;
    QProcess ps;
    ps.start(QStringLiteral("ps"), {QStringLiteral("-axo"), QStringLiteral("pid=,rss=,comm=")});
    if (!ps.waitForFinished(3000))
        return out;

    const QString text = QString::fromLocal8Bit(ps.readAllStandardOutput());
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    out.reserve(lines.size());
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        const int firstSpace = trimmed.indexOf(QLatin1Char(' '));
        if (firstSpace <= 0)
            continue;
        const QString pidText = trimmed.left(firstSpace);
        const QString rest = trimmed.mid(firstSpace + 1).trimmed();
        const int secondSpace = rest.indexOf(QLatin1Char(' '));
        if (secondSpace <= 0)
            continue;

        bool pidOk = false;
        bool rssOk = false;
        const quint32 pid = pidText.toUInt(&pidOk);
        const quint64 rssKb = rest.left(secondSpace).toULongLong(&rssOk);
        if (!pidOk || !rssOk)
            continue;

        ProcessMemory p;
        p.pid = pid;
        // comm 可能是带空格的完整路径，取末段当进程名
        const QString comm = rest.mid(secondSpace + 1).trimmed();
        p.name = comm.mid(comm.lastIndexOf(QLatin1Char('/')) + 1);
        if (p.name.isEmpty())
            p.name = comm;
        p.workingSet = rssKb * 1024;
        p.readable = true;
        out.append(p);
    }
    return out;
}

#endif // Q_OS_MACOS

} // namespace

SystemMemory readSystemMemory()
{
    SystemMemory mem;

#if defined(Q_OS_WIN)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        mem.totalBytes = status.ullTotalPhys;
        mem.availBytes = status.ullAvailPhys;
        mem.valid = mem.totalBytes > 0;
    }
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/meminfo"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        quint64 totalKb = 0;
        quint64 availKb = 0;
        quint64 freeKb = 0;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            if (line.startsWith("MemTotal:"))
                totalKb = parseKbLine(line);
            else if (line.startsWith("MemAvailable:"))
                availKb = parseKbLine(line);
            else if (line.startsWith("MemFree:"))
                freeKb = parseKbLine(line);
        }
        // MemAvailable 是 3.14 才有的；老内核退化成 MemFree（会低估可用量，但不会撒谎）
        if (availKb == 0)
            availKb = freeKb;
        mem.totalBytes = totalKb * 1024;
        mem.availBytes = availKb * 1024;
        mem.valid = mem.totalBytes > 0;
    }
#elif defined(Q_OS_MACOS)
    quint64 total = 0;
    size_t size = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0 && total > 0) {
        mem.totalBytes = total;
        // "可用"取 free + inactive：inactive 页可以随时被回收，活动监视器也算进可用
        vm_statistics64_data_t vm{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        vm_size_t pageSize = 0;
        if (host_page_size(mach_host_self(), &pageSize) == KERN_SUCCESS
            && host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                 reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
            const quint64 pages = static_cast<quint64>(vm.free_count) + vm.inactive_count
                + vm.speculative_count;
            mem.availBytes = qMin(pages * static_cast<quint64>(pageSize), mem.totalBytes);
        }
        mem.valid = true;
    }
#endif

    return mem;
}

QVector<ProcessMemory> listProcesses()
{
    QVector<ProcessMemory> out;

#if defined(Q_OS_WIN)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return out;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessMemory process;
            process.pid = entry.th32ProcessID;
            process.name = QString::fromWCharArray(entry.szExeFile);
            if (process.name.isEmpty())
                process.name = QStringLiteral("(pid %1)").arg(process.pid);

            // PROCESS_QUERY_LIMITED_INFORMATION 就够读内存统计（Vista+），
            // 比 QUERY_INFORMATION 能多打开一批受保护进程，也不要求提权。
            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid);
            if (handle) {
                PROCESS_MEMORY_COUNTERS_EX counters{};
                counters.cb = sizeof(counters);
                if (GetProcessMemoryInfo(handle,
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                         sizeof(counters))) {
                    process.workingSet = counters.WorkingSetSize;
                    process.readable = true;
                }
                CloseHandle(handle);
            }
            out.append(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#elif defined(Q_OS_LINUX)
    const quint64 pageSize = static_cast<quint64>(sysconf(_SC_PAGESIZE));
    const QStringList entries = QDir(QStringLiteral("/proc"))
                                    .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    out.reserve(entries.size());
    for (const QString &entry : entries) {
        bool ok = false;
        const quint32 pid = entry.toUInt(&ok);
        if (!ok || pid == 0)
            continue;
        const QString pidDir = QStringLiteral("/proc/") + entry;
        ProcessMemory process;
        process.pid = pid;
        process.name = readComm(pidDir, pid);
        quint64 workingSet = 0;
        if (readStatm(pidDir, pageSize, &workingSet)) {
            process.workingSet = workingSet;
            process.readable = true;
        }
        out.append(process);
    }
#elif defined(Q_OS_MACOS)
    out = listProcessesViaPs();
#endif

    return out;
}

TrimOutcome trimProcess(quint32 pid)
{
    TrimOutcome outcome;

#if defined(Q_OS_WIN)
    if (pid == 0 || pid == 4) {
        outcome.error = QStringLiteral("系统进程不允许回收");
        return outcome;
    }

    // EmptyWorkingSet 需要 PROCESS_SET_QUOTA + PROCESS_QUERY_INFORMATION。
    // 用不了 QUERY_LIMITED：那套只够读统计，刷工作集会被拒绝。
    HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_QUOTA, FALSE, pid);
    if (!handle) {
        const DWORD code = GetLastError();
        outcome.error = code == ERROR_ACCESS_DENIED
            ? QStringLiteral("没有权限（受保护的系统进程或其他用户的进程）")
            : QStringLiteral("无法打开进程（错误码 %1）").arg(code);
        return outcome;
    }

    PROCESS_MEMORY_COUNTERS_EX before{};
    before.cb = sizeof(before);
    if (!GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&before),
                              sizeof(before))) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        outcome.error = QStringLiteral("读取内存信息失败（错误码 %1）").arg(code);
        return outcome;
    }
    outcome.beforeBytes = before.WorkingSetSize;
    outcome.afterBytes = outcome.beforeBytes;   // 后面读不到就按"没变化"算

    if (!EmptyWorkingSet(handle)) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        outcome.error = QStringLiteral("回收失败（错误码 %1）").arg(code);
        return outcome;
    }

    PROCESS_MEMORY_COUNTERS_EX after{};
    after.cb = sizeof(after);
    if (GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&after),
                             sizeof(after)))
        outcome.afterBytes = after.WorkingSetSize;

    CloseHandle(handle);
    outcome.ok = true;
#else
    Q_UNUSED(pid)
    outcome.error = unsupportedTrimReason();
#endif

    return outcome;
}

bool canTrimOtherProcesses()
{
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

QString unsupportedTrimReason()
{
#if defined(Q_OS_WIN)
    return QString();
#elif defined(Q_OS_LINUX)
    return QStringLiteral("当前平台不支持：Linux 没有普通用户可用的"
                          "“刷出指定进程工作集”接口");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("当前平台不支持：macOS 未开放回收其他进程内存的接口"
                          "（purge 需要 root，且动的是系统缓存）");
#else
    return QStringLiteral("当前平台不支持回收其他进程的内存");
#endif
}

} // namespace memprobe

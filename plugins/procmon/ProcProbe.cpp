#include "ProcProbe.h"

// ---------------------------------------------------------------------------
// 平台实现。三段结构固定：Windows / Linux / macOS，其余平台走兜底分支。
//
// 注意：本文件是插件里唯一允许出现原生头文件的地方。往这里加东西时别忘了
// 其他两段也要有对应实现或明确的不支持文案。
// ---------------------------------------------------------------------------

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QProcess>
#include <QStringList>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <cstring>

#if defined(Q_OS_WIN)
// winsock2 必须在 windows.h 之前：iphlpapi.h 里的 MIB 结构依赖它的类型定义，
// 顺序反了会和 winsock.h 撞一堆重复定义。
#  include <winsock2.h>
// ws2ipdef.h 必须在 iphlpapi.h 之前：netioapi.h 里"接口管理"那一整段（MIB_IF_TABLE2 /
// GetIfTable2）包在 #ifdef _WS2IPDEF_ 里，而 iphlpapi.h 自己不引它 —— 不预先把
// _WS2IPDEF_ 定义上，编译期只会看到"未知类型"，看不出是头文件依赖缺失。
#  include <ws2ipdef.h>
#  include <windows.h>
#  include <iphlpapi.h>     // GetExtendedTcpTable / GetExtendedUdpTable
#  include <netioapi.h>     // GetIfTable2 / MIB_IF_TABLE2 / FreeMibTable（不在 iphlpapi.h 里）
#  include <psapi.h>
#  include <shellapi.h>     // CommandLineToArgvW：按 Windows 规则切分命令行
#  include <tlhelp32.h>     // CreateToolhelp32Snapshot：windows.h 不带，要单独引
#  include <winternl.h>     // UNICODE_STRING（读远程命令行要用）
#  include <string>
#elif defined(Q_OS_MACOS)
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <net/if_dl.h>
#  include <sys/socket.h>
#  include <sys/sysctl.h>
#  include <mach/mach.h>
#  include <unistd.h>
#  include <signal.h>
#  include <errno.h>
// if_data 的声明位置在各 SDK 里不完全一致（老的在 net/if.h，新的在 net/if_var.h）。
// 有就用真结构体，没有就退回"读前导字段"，别为了一个结构体让整段编不过。
#  if defined(__has_include)
#    if __has_include(<net/if_var.h>)
#      include <net/if_var.h>
#      define XTOOLS_HAVE_IF_DATA 1
#    endif
#  endif
#elif defined(Q_OS_LINUX)
#  include <unistd.h>   // sysconf(_SC_PAGESIZE / _SC_CLK_TCK)
#  include <signal.h>
#  include <errno.h>
#endif

namespace procprobe {

namespace {

#if defined(Q_OS_WIN)

/// MIB 表里的端口塞在 DWORD 低 16 位，而且是网络字节序。
/// 自己翻转，就不必为了 ntohs 去链 ws2_32。
quint16 portFromMib(DWORD value)
{
    const DWORD port = value & 0xFFFFu;
    return static_cast<quint16>(((port & 0xFFu) << 8) | ((port >> 8) & 0xFFu));
}

/// 把 MIB 表按 pid 归并成 "pid → 端口列表"。
///
/// 必须走 GetExtended*Table：只有内核的 owner 表里才有 dwOwningPid。逐进程去问
/// "你占了哪些端口"是拿不到的（那要 ETW 或逐个枚举句柄）。
QHash<quint32, QVector<quint16>> listeningPorts()
{
    QHash<quint32, QVector<quint16>> out;
    const auto add = [&out](quint32 pid, quint16 port) {
        // 端口 0 是"还没 bind 到具体端口"的占位条目（UDP 表里很常见），
        // 留着会让一堆进程显示 "0"，搜索也没有意义
        if (pid == 0 || port == 0)
            return;
        QVector<quint16> &ports = out[pid];
        if (!ports.contains(port))
            ports.append(port);
    };

    for (const ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        // ---- TCP：只要 LISTEN。已建立连接会带出一堆临时端口，搜索时全是噪音
        DWORD size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
            QByteArray buffer(static_cast<int>(size), '\0');
            if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family,
                                    TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
                const auto *table =
                    reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(buffer.constData());
                for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                    const MIB_TCPROW_OWNER_PID &row = table->table[i];
                    add(row.dwOwningPid, portFromMib(row.dwLocalPort));
                }
            }
        }

        // ---- UDP：没有 LISTEN 状态，表里的条目就是"已经绑定了这个端口"
        size = 0;
        if (GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
            QByteArray buffer(static_cast<int>(size), '\0');
            if (GetExtendedUdpTable(buffer.data(), &size, FALSE, family,
                                    UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
                const auto *table =
                    reinterpret_cast<const MIB_UDPTABLE_OWNER_PID *>(buffer.constData());
                for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                    const MIB_UDPROW_OWNER_PID &row = table->table[i];
                    add(row.dwOwningPid, portFromMib(row.dwLocalPort));
                }
            }
        }
    }

    for (auto it = out.begin(); it != out.end(); ++it)
        std::sort(it.value().begin(), it.value().end());
    return out;
}

/// FILETIME（100ns 为单位）转微秒。
quint64 fileTimeToUs(const FILETIME &time)
{
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart / 10;   // 100ns → us
}

/// 读远程进程内存的小包装。失败（受保护进程、刚退出）返回 false。
template <typename T>
bool readRemote(HANDLE process, quintptr address, T *out)
{
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), out, sizeof(T), &read)
        && read == sizeof(T);
}

/// NtQueryInformationProcess 的 ProcessBasicInformation：只为了拿 PEB 地址。
/// 它没有公开头文件，这里按文档声明最小签名 —— 少引一个 winternl.h 的坑。
using NtQueryInformationProcessFn = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct BasicProcessInfo
{
    LONG exitStatus;
    PVOID pebBase;
    ULONG_PTR affinityMask;
    LONG basePriority;
    ULONG_PTR uniquePid;
    ULONG_PTR inheritedPid;
};

/// PEB / RTL_USER_PROCESS_PARAMETERS 里几个字段的偏移。
///
/// 这两个结构没有稳定的公开布局，只能按位数写死常量。只在"同位数进程"之间读
/// （下面用 IsWow64Process 挡跨位数），所以常量与自身编译位数一致即可。
#if defined(_WIN64)
constexpr quintptr kPebParamsOffset = 0x20;        ///< PEB.ProcessParameters
constexpr quintptr kParamsCommandLineOffset = 0x70;///< RTL_USER_PROCESS_PARAMETERS.CommandLine
constexpr quintptr kParamsCurrentDirOffset = 0x38; ///< .CurrentDirectory（CURDIR{UNICODE_STRING DosPath; HANDLE}）
#else
constexpr quintptr kPebParamsOffset = 0x10;
constexpr quintptr kParamsCommandLineOffset = 0x40;
constexpr quintptr kParamsCurrentDirOffset = 0x24;
#endif

/// 从远程进程里把 UNICODE_STRING 指向的字符串读出来。
QString readRemoteString(HANDLE process, const UNICODE_STRING &str)
{
    if (!str.Buffer || str.Length == 0)
        return QString();
    QString text(static_cast<int>(str.Length / sizeof(QChar)), QChar());
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, str.Buffer, text.data(), str.Length, &read)
        || read != str.Length)
        return QString();
    return text;
}

#endif // Q_OS_WIN

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

/// 读 /proc/<pid>/statm 的第二个字段（resident），单位是页。
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

/// 读 /proc/<pid>/stat 里的 utime + stime（单位是时钟滴答），换算成微秒。
///
/// 不能直接 split(' ')：进程名（第 2 个字段）本身可能带空格和括号，所以从**最后
/// 一个 ')'** 之后才开始切字段。切完后 fields[0] 对应 stat 的第 3 个字段，
/// 于是 utime（第 14）落在 fields[11]，stime（第 15）落在 fields[12]。
bool readCpuTimeUs(const QString &pidDir, quint64 tickPerSec, quint64 *cpuUs)
{
    QFile stat(pidDir + QStringLiteral("/stat"));
    if (!stat.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QByteArray content = stat.readAll();
    const int close = content.lastIndexOf(')');
    if (close < 0)
        return false;
    const QList<QByteArray> fields = content.mid(close + 2).simplified().split(' ');
    if (fields.size() < 13)
        return false;
    bool userOk = false;
    bool kernelOk = false;
    const quint64 utime = fields.at(11).toULongLong(&userOk);
    const quint64 stime = fields.at(12).toULongLong(&kernelOk);
    if (!userOk || !kernelOk || tickPerSec == 0)
        return false;
    *cpuUs = (utime + stime) * 1000000ull / tickPerSec;
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

/// /proc/net/{tcp,tcp6,udp,udp6} → (socket inode → 本地端口)。
///
/// 只收 TCP 的 LISTEN（状态 0A）。UDP 没有状态机，表里的条目就是"绑定了端口"，
/// 全部收。
QHash<quint64, quint16> listeningInodePorts()
{
    QHash<quint64, quint16> out;
    const QStringList files{QStringLiteral("/proc/net/tcp"), QStringLiteral("/proc/net/tcp6"),
                            QStringLiteral("/proc/net/udp"), QStringLiteral("/proc/net/udp6")};
    for (const QString &path : files) {
        const bool tcp = path.endsWith(QStringLiteral("tcp"))
            || path.endsWith(QStringLiteral("tcp6"));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        file.readLine();   // 表头
        while (!file.atEnd()) {
            // 列：sl, local_address, rem_address, st, tx:rx, tr:when, retrnsmt, uid,
            //     timeout, inode, ...
            const QList<QByteArray> parts = file.readLine().simplified().split(' ');
            if (parts.size() < 10)
                continue;
            if (tcp && parts.at(3) != "0A")
                continue;
            const int colon = parts.at(1).lastIndexOf(':');
            if (colon < 0)
                continue;
            bool portOk = false;
            bool inodeOk = false;
            const quint16 port =
                static_cast<quint16>(parts.at(1).mid(colon + 1).toUShort(&portOk, 16));
            const quint64 inode = parts.at(9).toULongLong(&inodeOk);
            if (portOk && inodeOk && inode != 0)
                out.insert(inode, port);
        }
    }
    return out;
}

/// 某个进程占用的监听端口：扫 /proc/<pid>/fd，把 socket:[inode] 换成端口。
///
/// 内核没有 "pid → 端口" 的直接接口，只能反查：先有 inode→端口表（见上），
/// 再看每个进程打开了哪些 socket inode。每个进程一次 readdir，2 秒采样下够用。
QVector<quint16> portsForPid(const QString &pidDir, const QHash<quint64, quint16> &inodePorts)
{
    QVector<quint16> out;
    if (inodePorts.isEmpty())
        return out;
    const QString fdDir = pidDir + QStringLiteral("/fd");
    // socket 的 fd 链接不是普通文件，必须带 QDir::System 才列得出来
    const QStringList fds =
        QDir(fdDir).entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &fd : fds) {
        const QString target = QFile::symLinkTarget(fdDir + QLatin1Char('/') + fd);
        // 形如 "socket:[123456]"
        if (!target.startsWith(QStringLiteral("socket:[")) || !target.endsWith(QLatin1Char(']')))
            continue;
        bool ok = false;
        const quint64 inode = target.mid(8, target.size() - 9).toULongLong(&ok);
        if (!ok)
            continue;
        const auto it = inodePorts.constFind(inode);
        if (it != inodePorts.constEnd())
            out.append(it.value());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

#endif // Q_OS_LINUX

#if defined(Q_OS_MACOS)

/// 把 ps 的 cputime 字段转成秒。格式是 "M:SS.cc" 或 "H:MM:SS"。
double parseCpuTime(const QString &text)
{
    double seconds = 0.0;
    const QStringList parts = text.split(QLatin1Char(':'));
    for (const QString &part : parts) {
        bool ok = false;
        const double value = part.toDouble(&ok);
        if (!ok)
            return 0.0;
        seconds = seconds * 60.0 + value;
    }
    return seconds;
}

/// 用 `ps` 列进程。
///
/// 为什么不用 sysctl(KERN_PROC_ALL)：那套结构体在不同 macOS 版本上字段偏移变过，
/// 且要自己算 kinfo_proc 大小；`ps -axo` 的输出格式二十年没变，解析成本远低于维护
/// 一段脆弱的 ABI 代码。代价是一次进程外调用（约 10ms），面板 2 秒刷一次够用。
///
/// 用 cputime（累计 CPU 时间）而不是 pcpu（指数衰减的平均值）：这样三端口径一致，
/// 占用率都由两次采样差值算出。ps 的 cputime 只有 10ms 精度，2 秒窗口够用。
QVector<ProcessInfo> listProcessesViaPs()
{
    QVector<ProcessInfo> out;
    QProcess ps;
    ps.start(QStringLiteral("ps"),
             {QStringLiteral("-axo"), QStringLiteral("pid=,rss=,cputime=,comm=")});
    if (!ps.waitForFinished(3000))
        return out;

    const QString text = QString::fromLocal8Bit(ps.readAllStandardOutput());
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    out.reserve(lines.size());
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        // 前三个 token（pid / rss / cputime）不会有空格，剩下的全是 comm，可能带空格
        const int firstSpace = trimmed.indexOf(QLatin1Char(' '));
        if (firstSpace <= 0)
            continue;
        const QString pidText = trimmed.left(firstSpace);
        const QString rest = trimmed.mid(firstSpace + 1).trimmed();
        const int secondSpace = rest.indexOf(QLatin1Char(' '));
        if (secondSpace <= 0)
            continue;
        const QString rssText = rest.left(secondSpace);
        const QString rest2 = rest.mid(secondSpace + 1).trimmed();
        const int thirdSpace = rest2.indexOf(QLatin1Char(' '));
        if (thirdSpace <= 0)
            continue;
        const QString cpuText = rest2.left(thirdSpace);
        const QString comm = rest2.mid(thirdSpace + 1).trimmed();

        bool pidOk = false;
        bool rssOk = false;
        const quint32 pid = pidText.toUInt(&pidOk);
        const quint64 rssKb = rssText.toULongLong(&rssOk);
        if (!pidOk || !rssOk)
            continue;

        ProcessInfo process;
        process.pid = pid;
        // comm 可能是带空格的完整路径，取末段当进程名
        process.name = comm.mid(comm.lastIndexOf(QLatin1Char('/')) + 1);
        if (process.name.isEmpty())
            process.name = comm;
        // exePath 留空：comm 是"路径被截断过"的字段，拿去打开目录会指到错的地方
        process.workingSet = rssKb * 1024;
        process.memoryReadable = true;
        process.cpuTimeUs = static_cast<quint64>(parseCpuTime(cpuText) * 1e6);
        process.countersReadable = true;
        out.append(process);
    }
    return out;
}

/// 用 lsof 拿 "pid → 监听端口"。
///
/// 为什么是 lsof：macOS 没有 Windows 那种内核级 owner 表，`netstat -an` 又不带 pid。
/// lsof 随系统预装，-nP 关掉反解与端口名转换，输出好解析。用绝对路径是因为从
/// Finder / Dock 启动的 App 继承的 PATH 里常常没有 /usr/sbin。
QHash<quint32, QVector<quint16>> listeningPortsViaLsof()
{
    QHash<quint32, QVector<quint16>> out;
    QProcess lsof;
    lsof.start(QStringLiteral("/usr/sbin/lsof"),
               {QStringLiteral("-nP"), QStringLiteral("-iTCP"), QStringLiteral("-sTCP:LISTEN"),
                QStringLiteral("-iUDP")});
    if (!lsof.waitForFinished(3000))
        return out;

    const QString text = QString::fromLocal8Bit(lsof.readAllStandardOutput());
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList parts = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() < 9 || parts.at(0) == QStringLiteral("COMMAND"))
            continue;
        bool pidOk = false;
        const quint32 pid = parts.at(1).toUInt(&pidOk);
        if (!pidOk)
            continue;
        // NAME 是最后一列，形如 "*:8080" / "127.0.0.1:5353" / "[::1]:443"
        const QString name = parts.last();
        const int colon = name.lastIndexOf(QLatin1Char(':'));
        if (colon < 0 || colon == name.size() - 1)
            continue;
        bool portOk = false;
        const quint16 port = name.mid(colon + 1).toUShort(&portOk);
        if (!portOk)
            continue;
        QVector<quint16> &ports = out[pid];
        if (!ports.contains(port))
            ports.append(port);
    }
    for (auto it = out.begin(); it != out.end(); ++it)
        std::sort(it.value().begin(), it.value().end());
    return out;
}

#endif // Q_OS_MACOS

/// Unix 系（Linux / macOS）通用的"等进程消失"：轮询 kill(pid, 0)。
///
/// 没有子进程句柄可等（进程不是我们创建的），只能轮询。50ms 一次，够覆盖绝大
/// 多数进程的退出时间；超时当"还没退"，由调用方决定下一步。
bool waitProcessGone(quint32 pid, int timeoutMs)
{
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    const auto alive = [pid] {
        return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
    };
    for (int waited = 0; waited < timeoutMs; waited += 50) {
        if (!alive())
            return true;
        QThread::msleep(50);
    }
    return !alive();
#else
    Q_UNUSED(pid)
    Q_UNUSED(timeoutMs)
    return false;
#endif
}

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
        // "可用"取 free + inactive + speculative：inactive 页可以随时被回收，
        // 活动监视器也算进可用
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

SystemNet readSystemNet()
{
    SystemNet net;

#if defined(Q_OS_WIN)
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR && table) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2 &row = table->Table[i];
            // 回环不算：否则本机 127.0.0.1 的流量会把"网络"顶得毫无意义
            if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            net.rxBytes += row.InOctets;
            net.txBytes += row.OutOctets;
        }
        net.valid = true;
        FreeMibTable(table);
    }
#elif defined(Q_OS_LINUX)
    QFile file(QStringLiteral("/proc/net/dev"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        file.readLine();   // 两行表头
        file.readLine();
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed();
            if (name == "lo")   // 同上：回环不计
                continue;
            // 冒号后是 16 个计数：rx bytes/packets/..., tx bytes/packets/...
            const QList<QByteArray> fields = line.mid(colon + 1).simplified().split(' ');
            if (fields.size() < 9)
                continue;
            bool rxOk = false;
            bool txOk = false;
            const quint64 rx = fields.at(0).toULongLong(&rxOk);
            const quint64 tx = fields.at(8).toULongLong(&txOk);
            if (rxOk && txOk) {
                net.rxBytes += rx;
                net.txBytes += tx;
                net.valid = true;
            }
        }
    }
#elif defined(Q_OS_MACOS)
    struct ifaddrs *head = nullptr;
    if (getifaddrs(&head) == 0) {
        for (struct ifaddrs *ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK || !ifa->ifa_data)
                continue;
            if (qstrcmp(ifa->ifa_name, "lo0") == 0)   // 回环不计
                continue;
#  if defined(XTOOLS_HAVE_IF_DATA)
            const auto *data = static_cast<const struct if_data *>(ifa->ifa_data);
            net.rxBytes += data->ifi_ibytes;
            net.txBytes += data->ifi_obytes;
#  else
            // 没有 if_var.h 时按 BSD if_data 的前导布局读：8 个 u_char 之后是
            // mtu/metric/baudrate/ipackets/ierrors/opackets/oerrors/collisions 八个
            // u_int32，紧接 ifi_ibytes(40) 与 ifi_obytes(44)。这段布局从 4.4BSD 起没变过。
            const auto *data = static_cast<const unsigned char *>(ifa->ifa_data);
            quint32 inBytes = 0;
            quint32 outBytes = 0;
            std::memcpy(&inBytes, data + 40, sizeof(inBytes));
            std::memcpy(&outBytes, data + 44, sizeof(outBytes));
            net.rxBytes += inBytes;
            net.txBytes += outBytes;
#  endif
            net.valid = true;
        }
        freeifaddrs(head);
    }
#endif

    return net;
}

QVector<ProcessInfo> listProcesses()
{
    QVector<ProcessInfo> out;

#if defined(Q_OS_WIN)
    const QHash<quint32, QVector<quint16>> portMap = listeningPorts();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return out;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcessInfo process;
            process.pid = entry.th32ProcessID;
            process.name = QString::fromWCharArray(entry.szExeFile);
            if (process.name.isEmpty())
                process.name = QStringLiteral("(pid %1)").arg(process.pid);
            process.ports = portMap.value(process.pid);

            // PROCESS_QUERY_LIMITED_INFORMATION 就够读时间/内存/路径（Vista+），
            // 比 QUERY_INFORMATION 能多打开一批受保护进程，也不要求提权。
            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.pid);
            if (handle) {
                FILETIME creation{};
                FILETIME exit{};
                FILETIME kernel{};
                FILETIME user{};
                if (GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
                    process.cpuTimeUs = fileTimeToUs(kernel) + fileTimeToUs(user);
                    process.countersReadable = true;
                }

                PROCESS_MEMORY_COUNTERS_EX counters{};
                counters.cb = sizeof(counters);
                if (GetProcessMemoryInfo(handle,
                                         reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                         sizeof(counters))) {
                    process.workingSet = counters.WorkingSetSize;
                    process.memoryReadable = true;
                }

                IO_COUNTERS io{};
                if (GetProcessIoCounters(handle, &io)) {
                    process.diskReadBytes = io.ReadTransferCount;
                    process.diskWriteBytes = io.WriteTransferCount;
                }

                // 路径按 32768 取：长路径与符号链接都能超过 MAX_PATH
                std::wstring path(32768, L'\0');
                DWORD length = static_cast<DWORD>(path.size());
                if (QueryFullProcessImageNameW(handle, 0, path.data(), &length))
                    process.exePath =
                        QString::fromWCharArray(path.c_str(), static_cast<int>(length));

                CloseHandle(handle);
            }
            out.append(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#elif defined(Q_OS_LINUX)
    const quint64 pageSize = static_cast<quint64>(sysconf(_SC_PAGESIZE));
    const quint64 tickPerSec = static_cast<quint64>(sysconf(_SC_CLK_TCK));
    const QHash<quint64, quint16> inodePorts = listeningInodePorts();
    const QStringList entries = QDir(QStringLiteral("/proc"))
                                    .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    out.reserve(entries.size());
    for (const QString &entry : entries) {
        bool ok = false;
        const quint32 pid = entry.toUInt(&ok);
        if (!ok || pid == 0)
            continue;
        const QString pidDir = QStringLiteral("/proc/") + entry;
        ProcessInfo process;
        process.pid = pid;
        process.name = readComm(pidDir, pid);

        quint64 workingSet = 0;
        if (readStatm(pidDir, pageSize, &workingSet)) {
            process.workingSet = workingSet;
            process.memoryReadable = true;
        }

        quint64 cpuUs = 0;
        if (readCpuTimeUs(pidDir, tickPerSec, &cpuUs)) {
            process.cpuTimeUs = cpuUs;
            process.countersReadable = true;
        }

        // /proc/<pid>/io 只有同用户（或 root）读得到，别的用户的进程会被拒绝，
        // 这时磁盘列显示 "—"，不影响其余列
        QFile io(pidDir + QStringLiteral("/io"));
        if (io.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!io.atEnd()) {
                const QByteArray line = io.readLine();
                const int colon = line.indexOf(':');
                if (colon <= 0)
                    continue;
                bool valueOk = false;
                const quint64 value = line.mid(colon + 1).trimmed().toULongLong(&valueOk);
                if (!valueOk)
                    continue;
                if (line.startsWith("read_bytes:"))
                    process.diskReadBytes = value;
                else if (line.startsWith("write_bytes:"))
                    process.diskWriteBytes = value;
            }
        }

        process.exePath = QFile::symLinkTarget(pidDir + QStringLiteral("/exe"));
        process.ports = portsForPid(pidDir, inodePorts);
        out.append(process);
    }
#elif defined(Q_OS_MACOS)
    out = listProcessesViaPs();
    const QHash<quint32, QVector<quint16>> portMap = listeningPortsViaLsof();
    for (ProcessInfo &process : out)
        process.ports = portMap.value(process.pid);
    // 磁盘 I/O：macOS 没有按进程统计的稳定接口，面板据 canReadDiskCounters()
    // 显示 "—"（CPU 时间本身可信，countersReadable 保持 true）
#endif

    return out;
}

ProcessCommand readCommand(quint32 pid)
{
    ProcessCommand cmd;

#if defined(Q_OS_WIN)
    if (pid == 0 || pid == 4) {
        cmd.error = QStringLiteral("系统进程没有可重启的命令行");
        return cmd;
    }

    // 要读 PEB 就必须有 VM_READ；提权进程会直接拒绝，于是这里失败、上面如实报错
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!handle) {
        const DWORD code = GetLastError();
        cmd.error = code == ERROR_ACCESS_DENIED
            ? QStringLiteral("没有权限读取该进程的命令行（受保护进程或其他用户的进程）")
            : QStringLiteral("无法打开进程（错误码 %1）").arg(code);
        return cmd;
    }

    // 32 位进程的 PEB 用 32 位指针，64 位进程去读只会拿到垃圾地址 —— 位数不同就放弃
    BOOL selfWow = FALSE;
    BOOL targetWow = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfWow);
    IsWow64Process(handle, &targetWow);
    if (selfWow != targetWow) {
        CloseHandle(handle);
        cmd.error = QStringLiteral("该进程与本程序的位数不同（32/64 位），读不到命令行");
        return cmd;
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto ntQuery = reinterpret_cast<NtQueryInformationProcessFn>(
        ntdll ? GetProcAddress(ntdll, "NtQueryInformationProcess") : nullptr);
    if (!ntQuery) {
        CloseHandle(handle);
        cmd.error = QStringLiteral("系统未提供 NtQueryInformationProcess，读不到命令行");
        return cmd;
    }

    BasicProcessInfo basic{};
    if (ntQuery(handle, 0 /* ProcessBasicInformation */, &basic, sizeof(basic), nullptr) != 0
        || !basic.pebBase) {
        CloseHandle(handle);
        cmd.error = QStringLiteral("读不到进程的 PEB，无法取得命令行");
        return cmd;
    }

    PVOID params = nullptr;
    if (!readRemote(handle, reinterpret_cast<quintptr>(basic.pebBase) + kPebParamsOffset, &params)
        || !params) {
        CloseHandle(handle);
        cmd.error = QStringLiteral("读不到进程参数块，无法取得命令行");
        return cmd;
    }

    const quintptr paramsAddr = reinterpret_cast<quintptr>(params);
    UNICODE_STRING commandLine{};
    if (!readRemote(handle, paramsAddr + kParamsCommandLineOffset, &commandLine)) {
        CloseHandle(handle);
        cmd.error = QStringLiteral("读不到命令行字段");
        return cmd;
    }
    const QString text = readRemoteString(handle, commandLine);
    if (text.isEmpty()) {
        CloseHandle(handle);
        cmd.error = QStringLiteral("命令行是空的（进程可能刚启动或已被保护）");
        return cmd;
    }

    // 工作目录读不到不影响重启（退回当前目录），所以失败时不报错
    UNICODE_STRING currentDir{};
    if (readRemote(handle, paramsAddr + kParamsCurrentDirOffset, &currentDir))
        cmd.workingDir = readRemoteString(handle, currentDir);

    // 顺手把真实 exe 路径读出来：argv[0] 往往是"用户当初怎么敲的"（比如 "ping"），
    // 重启时按 PATH 再找一次容易启动到另一个版本，能换成绝对路径就换
    QString exePath;
    {
        std::wstring buffer(32768, L'\0');
        DWORD length = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(handle, 0, buffer.data(), &length))
            exePath = QString::fromWCharArray(buffer.c_str(), static_cast<int>(length));
    }

    CloseHandle(handle);

    // CommandLineToArgvW 是 Windows 官方的 argv 切分规则（引号、反斜杠转义都按它来），
    // 自己写一个必然在某些路径上出错
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(text.utf16()), &argc);
    if (!argv || argc == 0) {
        if (argv)
            LocalFree(argv);
        cmd.error = QStringLiteral("命令行解析失败");
        return cmd;
    }
    cmd.program = QString::fromWCharArray(argv[0]);
    for (int i = 1; i < argc; ++i)
        cmd.args.append(QString::fromWCharArray(argv[i]));
    LocalFree(argv);

    // 裸程序名（相对 PATH / 当前目录）换成刚才读到的绝对路径
    if (!exePath.isEmpty() && !cmd.program.contains(QLatin1Char('\\'))
        && !cmd.program.contains(QLatin1Char('/')))
        cmd.program = exePath;

    cmd.valid = !cmd.program.isEmpty();
    if (!cmd.valid)
        cmd.error = QStringLiteral("命令行里没有可执行程序");
    return cmd;

#elif defined(Q_OS_LINUX)
    QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!cmdline.open(QIODevice::ReadOnly)) {
        cmd.error = QStringLiteral("读不到 /proc/%1/cmdline（进程可能已退出）").arg(pid);
        return cmd;
    }
    const QByteArray raw = cmdline.readAll();
    // cmdline 用 '\0' 分隔参数，末尾还有一个 '\0'；中间的空参数要保留
    QList<QByteArray> parts = raw.split('\0');
    while (!parts.isEmpty() && parts.last().isEmpty())
        parts.removeLast();
    if (parts.isEmpty()) {
        cmd.error = QStringLiteral("该进程没有命令行（可能是内核线程）");
        return cmd;
    }
    cmd.program = QString::fromUtf8(parts.first());
    for (int i = 1; i < parts.size(); ++i)
        cmd.args.append(QString::fromUtf8(parts.at(i)));
    cmd.workingDir = QFile::symLinkTarget(QStringLiteral("/proc/%1/cwd").arg(pid));
    cmd.valid = !cmd.program.isEmpty();
    if (!cmd.valid)
        cmd.error = QStringLiteral("命令行里没有可执行程序");
    return cmd;

#elif defined(Q_OS_MACOS)
    QProcess ps;
    ps.start(QStringLiteral("ps"), {QStringLiteral("-o"), QStringLiteral("command="),
                                    QStringLiteral("-p"), QString::number(pid)});
    if (!ps.waitForFinished(2000)) {
        cmd.error = QStringLiteral("ps 调用超时，读不到命令行");
        return cmd;
    }
    const QString text = QString::fromLocal8Bit(ps.readAllStandardOutput()).trimmed();
    if (text.isEmpty()) {
        cmd.error = QStringLiteral("读不到该进程的命令行（可能已退出）");
        return cmd;
    }
    // ps 给的是拼好的整串，交给 Qt 的 splitCommand 按 POSIX 规则切回 program + args
    const QStringList tokens = QProcess::splitCommand(text);
    if (tokens.isEmpty()) {
        cmd.error = QStringLiteral("命令行解析失败");
        return cmd;
    }
    cmd.program = tokens.first();
    for (int i = 1; i < tokens.size(); ++i)
        cmd.args.append(tokens.at(i));
    // 工作目录留空：macOS 没有 /proc，取 cwd 要再起一次 lsof，收益不值这个成本
    // （QProcess::startDetached 会沿用本程序当前目录）
    cmd.valid = true;
    return cmd;

#else
    Q_UNUSED(pid)
    cmd.error = QStringLiteral("当前平台不支持读取进程命令行");
    return cmd;
#endif
}

KillOutcome killProcess(quint32 pid)
{
    KillOutcome outcome;

#if defined(Q_OS_WIN)
    if (pid == 0 || pid == 4) {
        outcome.error = QStringLiteral("系统进程不允许结束");
        return outcome;
    }
    if (pid == GetCurrentProcessId()) {
        outcome.error = QStringLiteral("不能结束本程序自己的进程");
        return outcome;
    }

    // SYNCHRONIZE 是为了下面能被 WaitForSingleObject 等住：不等就返回，调用方紧接着
    // 重启会撞上"旧进程还占着端口/文件"
    HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, pid);
    if (!handle) {
        const DWORD code = GetLastError();
        outcome.error = code == ERROR_ACCESS_DENIED
            ? QStringLiteral("没有权限（受保护的系统进程、其他用户的进程或需要管理员）")
            : QStringLiteral("无法打开进程（错误码 %1）").arg(code);
        return outcome;
    }

    if (!TerminateProcess(handle, 1)) {
        const DWORD code = GetLastError();
        CloseHandle(handle);
        outcome.error = QStringLiteral("结束进程失败（错误码 %1）").arg(code);
        return outcome;
    }
    // 3 秒足够绝大多数进程退出；超时也不算失败（可能卡在驱动里），刷新就能看到实况
    WaitForSingleObject(handle, 3000);
    CloseHandle(handle);
    outcome.ok = true;
    return outcome;

#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    if (pid <= 1) {
        outcome.error = QStringLiteral("系统进程不允许结束");
        return outcome;
    }
    if (static_cast<qint64>(pid) == static_cast<qint64>(getpid())) {
        outcome.error = QStringLiteral("不能结束本程序自己的进程");
        return outcome;
    }

    // 先 SIGTERM 让进程有机会存盘退出；等不到再 SIGKILL。直接 SIGKILL 会丢掉未落盘
    // 的数据，对一个"顺手点点"的按钮太粗暴。
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
        outcome.error = (errno == EPERM)
            ? QStringLiteral("没有权限（其他用户的进程，可能需要 root）")
            : (errno == ESRCH ? QStringLiteral("进程已经不存在了")
                              : QStringLiteral("发送 SIGTERM 失败（errno %1）").arg(errno));
        return outcome;
    }
    if (waitProcessGone(pid, 1500)) {
        outcome.ok = true;
        return outcome;
    }

    if (::kill(static_cast<pid_t>(pid), SIGKILL) != 0) {
        outcome.error = (errno == EPERM)
            ? QStringLiteral("进程未响应 SIGTERM，且没有权限强制结束")
            : QStringLiteral("强制结束失败（errno %1）").arg(errno);
        return outcome;
    }
    if (!waitProcessGone(pid, 1000)) {
        outcome.error = QStringLiteral("已发送 SIGKILL，但进程仍未退出");
        return outcome;
    }
    outcome.ok = true;
    return outcome;

#else
    Q_UNUSED(pid)
    outcome.error = QStringLiteral("当前平台不支持结束进程");
    return outcome;
#endif
}

bool revealProcessFile(const QString &exePath, QString *error)
{
    if (exePath.isEmpty()) {
        if (error)
            *error = QStringLiteral("读不到该进程的可执行文件路径");
        return false;
    }

#if defined(Q_OS_WIN)
    // explorer 自己解析参数，"/select," 后面必须紧跟路径（中间不能有空格）
    const QString argument = QStringLiteral("/select,") + QDir::toNativeSeparators(exePath);
    if (QProcess::startDetached(QStringLiteral("explorer.exe"), {argument}))
        return true;
    if (error)
        *error = QStringLiteral("无法打开文件管理器");
    return false;
#elif defined(Q_OS_MACOS)
    // -R 是"在访达里显示"，比直接 open 目录更接近用户的期待
    if (QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), exePath}))
        return true;
    if (error)
        *error = QStringLiteral("无法调用 open -R");
    return false;
#else
    const QFileInfo info(exePath);
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath())))
        return true;
    if (error)
        *error = QStringLiteral("无法打开文件管理器（桌面环境可能缺少 xdg-open）");
    return false;
#endif
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

bool canReadDiskCounters()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

QString unsupportedDiskReason()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return QString();
#else
    return QStringLiteral("当前平台不提供按进程统计磁盘 I/O 的接口");
#endif
}

} // namespace procprobe

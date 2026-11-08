#include "FileLockProbe.h"

// ---------------------------------------------------------------------------
// 平台实现。结构固定为 Windows / Linux / macOS / 兜底四段，任何原生调用都
// 包在对应宏里。加东西时另外三段要么有实现、要么有明确的可读失败原因。
// ---------------------------------------------------------------------------

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStringList>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <restartmanager.h>   // RmStartSession / RmRegisterResources / RmGetList
#  include <string>
#  include <vector>
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#  include <signal.h>    // kill / SIGTERM 是 POSIX 而非 ISO C，<csignal> 不保证声明
#  include <cerrno>
#  include <cstring>     // std::strerror
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace fileprobe {
namespace {

// 路径比较是否忽略大小写由平台决定；纯逻辑层 pathKey() 只认这个参数，
// 不自己写 #if，这样临时工程能同时测两种语义。
#if defined(Q_OS_WIN)
constexpr bool kCaseInsensitive = true;
#elif defined(Q_OS_LINUX)
constexpr bool kCaseInsensitive = false;
#elif defined(Q_OS_MACOS)
constexpr bool kCaseInsensitive = false;
#else
constexpr bool kCaseInsensitive = false;
#endif

#if defined(Q_OS_WIN)

/// Win32 错误码 → 系统给的可读文案。
/// 只写错误码用户没法自查（尤其 ACCESS_DENIED 与 SHARING_VIOLATION 的
/// 处理方式完全不同），所以宁可多一次 FormatMessage。
QString winErrorMessage(DWORD code)
{
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || !buffer)
        return QStringLiteral("错误码 %1").arg(code);
    const QString text = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    LocalFree(buffer);
    return text.isEmpty() ? QStringLiteral("错误码 %1").arg(code) : text;
}

/// RM_APP_TYPE → 中文类型名。表格「类型」列直接显示这个。
QString appTypeName(RM_APP_TYPE type)
{
    switch (type) {
    case RmMainWindow:
    case RmOtherWindow:  return QStringLiteral("窗口程序");
    case RmService:      return QStringLiteral("服务");
    case RmExplorer:     return QStringLiteral("资源管理器");
    case RmConsole:      return QStringLiteral("控制台");
    case RmCritical:     return QStringLiteral("系统关键进程");
    default:             return QStringLiteral("后台进程");
    }
}

/// Restart Manager 给的 strAppName 偶尔是空串（服务、无窗口进程），
/// 这时用 QueryFullProcessImageNameW 补一个可执行文件名。
/// 权限只申请 QUERY_LIMITED_INFORMATION，比 QUERY_INFORMATION 能多打开一批进程。
QString processName(quint64 pid)
{
    if (pid == 0 || pid > 0xFFFFFFFFull)
        return QString();
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(pid));
    if (!handle)
        return QString();
    WCHAR buffer[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    QString name;
    if (QueryFullProcessImageNameW(handle, 0, buffer, &size)) {
        name = QString::fromWCharArray(buffer, static_cast<int>(size));
        name = name.mid(name.lastIndexOf(QLatin1Char('\\')) + 1);
    }
    CloseHandle(handle);
    return name;
}

/// RmStartSession / RmEndSession 必须成对，异常分支也不能漏。
class RmSession
{
public:
    explicit RmSession(QString *error) : m_error(error)
    {
        WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
        const DWORD rc = RmStartSession(&m_handle, 0, key);
        if (rc != ERROR_SUCCESS) {
            *m_error = QStringLiteral("启动占用检测会话失败：%1").arg(winErrorMessage(rc));
            return;
        }
        m_open = true;
    }

    ~RmSession()
    {
        if (m_open)
            RmEndSession(m_handle);
    }

    RmSession(const RmSession &) = delete;
    RmSession &operator=(const RmSession &) = delete;

    bool ok() const { return m_open; }
    DWORD handle() const { return m_handle; }

private:
    QString *m_error = nullptr;
    DWORD m_handle = 0;
    bool m_open = false;
};

/// Windows 检测：Restart Manager。
///
/// 为什么不用 NtQuerySystemInformation 枚举全系统句柄来「检测」：那样每个句柄
/// 都要 DuplicateHandle + 查名字，几万个句柄起步、还容易在管道句柄上卡住。
/// RM 是微软给安装程序提供的官方接口，直接按文件查占用者，快且稳。
/// 代价是它只给进程、不给具体句柄 —— 所以「解除占用」另走一遍句柄枚举，
/// 且只在用户选中了 pid 之后按 pid 过滤，规模小得多。
QVector<filelock::Holder> scanWindows(const filelock::Targets &targets,
                                      quint64 selfPid, QString *error)
{
    QVector<filelock::Holder> out;
    if (targets.files.isEmpty())
        return out;   // 空文件夹没有文件可注册，不报错，状态栏会显示「没有占用」

    RmSession session(error);
    if (!session.ok())
        return out;

    // 先把 QString 全部转成稳定的 std::wstring，再收集指针。
    // 直接在循环里 push_back(c_str()) 会因 vector 扩容让先前的指针全部悬空。
    std::vector<std::wstring> wide;
    wide.reserve(static_cast<size_t>(targets.files.size()));
    for (const QString &file : targets.files)
        wide.push_back(file.toStdWString());
    std::vector<LPCWSTR> pointers;
    pointers.reserve(wide.size());
    for (const std::wstring &text : wide)
        pointers.push_back(text.c_str());

    DWORD rc = RmRegisterResources(session.handle(),
                                   static_cast<UINT>(pointers.size()), pointers.data(),
                                   0, nullptr, 0, nullptr);
    if (rc != ERROR_SUCCESS) {
        *error = QStringLiteral("注册待检测文件失败：%1").arg(winErrorMessage(rc));
        return out;
    }

    // RmGetList 的两段式：先问需要多少个槽位（返回 ERROR_MORE_DATA），
    // 再按这个数量重查。只调一次会拿不到完整列表。
    UINT needed = 0;
    UINT count = 0;
    DWORD rebootReasons = 0;
    rc = RmGetList(session.handle(), &needed, &count, nullptr, &rebootReasons);
    if (rc == ERROR_MORE_DATA && needed == 0)
        return out;   // 没有占用者
    std::vector<RM_PROCESS_INFO> infos;
    if (rc == ERROR_MORE_DATA) {
        infos.resize(needed);
        count = needed;
        rc = RmGetList(session.handle(), &needed, &count, infos.data(), &rebootReasons);
    }
    if (rc != ERROR_SUCCESS) {
        *error = QStringLiteral("查询占用进程失败：%1").arg(winErrorMessage(rc));
        return out;
    }

    const QString hit = targets.files.size() == 1
                            ? targets.files.first()
                            : QStringLiteral("%1 个文件").arg(targets.files.size());
    for (UINT i = 0; i < count && i < infos.size(); ++i) {
        const RM_PROCESS_INFO &info = infos[i];
        const quint64 pid = info.Process.dwProcessId;
        if (pid == 0 || pid == selfPid)
            continue;   // 绝不把宿主自己列进可操作对象
        filelock::Holder holder;
        holder.pid = pid;
        holder.name = QString::fromWCharArray(info.strAppName);
        if (holder.name.isEmpty())
            holder.name = processName(pid);
        holder.type = appTypeName(info.ApplicationType);
        holder.hitPath = hit;
        out.append(holder);
    }
    return out;
}

// SystemExtendedHandleInformation = 64（Vista+）。
// 这不是 SDK 公开结构体，所以自己按 ntdll 的布局定义；字段布局从 Win7 到 Win11
// 都没变过，是 Process Explorer / handle.exe 一脉相承的写法。
constexpr int kSystemExtendedHandleInformation = 64;
constexpr ULONG kStatusInfoLengthMismatch = 0xC0000004UL;

struct SystemHandleEntryEx
{
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct SystemHandleInfoEx
{
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SystemHandleEntryEx Handles[1];
};

using NtQuerySystemInformationFn = LONG(NTAPI *)(int, PVOID, ULONG, PULONG);

/// Windows「解除占用」：把选中进程里指向目标文件的文件句柄关掉。
///
/// 流程：枚举全系统句柄 → 只保留选中 pid → DuplicateHandle 把句柄复制到本进程 →
/// GetFinalPathNameByHandleW 拿到 DOS 路径 → 命中目标就用
/// DuplicateHandle(DUPLICATE_CLOSE_SOURCE) 关掉源句柄。
///
/// 刻意不用 NtQueryObject(ObjectNameInformation)：它对同步管道句柄会永久阻塞，
/// 且返回的是 \Device\HarddiskVolumeX\... 还要自己做卷映射。
/// GetFinalPathNameByHandle 直接给 \\?\C:\... 且不会挂住，路径转换在
/// FileLockLogic.h 的 fromWin32FinalPath() 里（可单测）。
///
/// 风险（UI 里也写明了）：被关掉句柄的进程通常不会立刻退出，但它下一次读写
/// 这个文件会拿到无效句柄，可能报错、崩溃或丢未保存数据。
filelock::ActionOutcome closeRemoteHandles(const filelock::Targets &targets,
                                           const QVector<quint64> &pids,
                                           quint64 selfPid, bool recursive)
{
    filelock::ActionOutcome outcome;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        outcome.error = QStringLiteral("无法加载 ntdll.dll");
        return outcome;
    }
    auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    if (!query) {
        outcome.error = QStringLiteral("当前系统不提供 NtQuerySystemInformation");
        return outcome;
    }

    // 句柄表大小随系统里进程数变化，用 STATUS_INFO_LENGTH_MISMATCH 反复扩容。
    // 64 MB 封顶，避免异常系统里死循环。
    std::vector<unsigned char> buffer;
    ULONG size = 1u << 20;
    LONG status = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        buffer.resize(size);
        ULONG needed = 0;
        status = query(kSystemExtendedHandleInformation, buffer.data(), size, &needed);
        if (status == 0)
            break;
        if (status != static_cast<LONG>(kStatusInfoLengthMismatch)) {
            outcome.error = QStringLiteral("枚举系统句柄失败（NTSTATUS 0x%1）")
                                .arg(QString::number(static_cast<quint32>(status), 16));
            return outcome;
        }
        size = needed > size ? needed + (1u << 16) : size * 2;
        if (size > (64u << 20)) {
            outcome.error = QStringLiteral("系统句柄表过大，已放弃枚举");
            return outcome;
        }
    }
    if (status != 0) {
        outcome.error = QStringLiteral("枚举系统句柄失败（NTSTATUS 0x%1）")
                            .arg(QString::number(static_cast<quint32>(status), 16));
        return outcome;
    }

    const auto *table = reinterpret_cast<const SystemHandleInfoEx *>(buffer.data());
    const filelock::MatchSet match = filelock::buildMatchSet(targets, true);
    const QSet<quint64> wanted(pids.begin(), pids.end());

    // 每个 pid 只 OpenProcess 一次；PROCESS_DUP_HANDLE 是复制/关闭句柄所需的最小权限。
    QHash<quint64, HANDLE> processHandles;
    QSet<quint64> touched;
    QSet<quint64> denied;
    int closed = 0;

    const ULONG_PTR total = table->NumberOfHandles;
    for (ULONG_PTR i = 0; i < total; ++i) {
        const SystemHandleEntryEx &entry = table->Handles[i];
        const quint64 pid = static_cast<quint64>(entry.UniqueProcessId);
        if (pid == 0 || pid == selfPid || !wanted.contains(pid)
            || pid > 0xFFFFFFFFull) {
            continue;
        }

        auto found = processHandles.constFind(pid);
        HANDLE process = nullptr;
        if (found == processHandles.constEnd()) {
            process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(pid));
            processHandles.insert(pid, process);   // 失败也记 nullptr，别反复重试
        } else {
            process = found.value();
        }
        if (!process) {
            denied.insert(pid);
            continue;
        }

        HANDLE source = reinterpret_cast<HANDLE>(entry.HandleValue);
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process, source, GetCurrentProcess(), &duplicate,
                             0, FALSE, 0)) {
            continue;   // 句柄可能在快照与现在之间已被关掉，或类型不允许复制
        }

        WCHAR pathBuffer[32768] = {};
        const DWORD length = GetFinalPathNameByHandleW(
            duplicate, pathBuffer, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        const QString opened = (length > 0 && length < 32768)
                                   ? filelock::fromWin32FinalPath(
                                         QString::fromWCharArray(pathBuffer,
                                                                 static_cast<int>(length)))
                                   : QString();
        CloseHandle(duplicate);   // 副本不参与占用判定，先关掉

        if (opened.isEmpty()
            || !filelock::matchesPath(filelock::pathKey(opened, true), match,
                                      recursive, true)) {
            continue;
        }

        // DUPLICATE_CLOSE_SOURCE 在复制的同时关掉对方进程里的原句柄；
        // 得到的副本留在我们进程，必须再 CloseHandle 一次，否则文件仍被占着。
        HANDLE closedCopy = nullptr;
        if (DuplicateHandle(process, source, GetCurrentProcess(), &closedCopy,
                            0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
            if (closedCopy)
                CloseHandle(closedCopy);
            ++closed;
            touched.insert(pid);
        } else {
            denied.insert(pid);
        }
    }

    for (auto it = processHandles.cbegin(); it != processHandles.cend(); ++it) {
        if (it.value())
            CloseHandle(it.value());
    }

    outcome.succeededPids = QVector<quint64>(touched.begin(), touched.end());
    outcome.closedHandles = closed;
    // 只对「确实想动、却一个句柄都没动成」的进程计失败，避免把
    // 「进程已经退出/自己释放了」误报成权限错误。
    for (quint64 pid : pids) {
        if (!touched.contains(pid) && denied.contains(pid))
            ++outcome.failed;
    }
    if (closed == 0 && denied.isEmpty())
        outcome.error = QStringLiteral("没有找到仍指向目标文件的句柄（可能已被释放）");
    else if (closed == 0 && !denied.isEmpty())
        outcome.error = QStringLiteral("无法打开这些进程的句柄（权限不足，"
                                       "请以管理员身份重试）");
    return outcome;
}

#endif // Q_OS_WIN

#if defined(Q_OS_LINUX)

/// /proc/<pid>/comm 是单行进程名，读不到就留给 mergeHolders 填 "(pid N)"。
QString readComm(quint64 pid)
{
    QFile file(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll()).trimmed();
}

/// Linux 检测：遍历 /proc/<pid>/fd 的符号链接目标。
///
/// procfs 是稳定的内核接口，读 fd 链接只要同用户权限；被删除但仍打开的文件
/// 目标会带 " (deleted)" 后缀，去掉后再比，否则永远匹配不上。
/// 不需要 lsof/fuser 这类外部命令，也就没有「命令没装」的失败分支。
QVector<filelock::Holder> scanProcFs(const filelock::Targets &targets,
                                     bool recursive, quint64 selfPid, QString *error)
{
    const filelock::MatchSet match = filelock::buildMatchSet(targets, false);
    QVector<filelock::Holder> out;
    int unreadable = 0;

    const QStringList entries = QDir(QStringLiteral("/proc"))
                                    .entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                               QDir::Name);
    for (const QString &entry : entries) {
        bool ok = false;
        const quint64 pid = entry.toULongLong(&ok);
        if (!ok || pid == 0 || pid == selfPid)
            continue;

        const QDir fdDir(QStringLiteral("/proc/%1/fd").arg(entry));
        if (!fdDir.exists()) {
            ++unreadable;   // 其他用户的进程通常读不到，如实告诉用户结果可能不全
            continue;
        }
        const QStringList fds = fdDir.entryList(QDir::Files | QDir::Dirs
                                                    | QDir::Hidden | QDir::System
                                                    | QDir::NoDotAndDotDot);
        int hits = 0;
        QString firstPath;
        for (const QString &fd : fds) {
            QString target = QFileInfo(fdDir.filePath(fd)).symLinkTarget();
            if (target.isEmpty())
                continue;
            if (target.endsWith(QLatin1String(" (deleted)")))
                target.chop(10);   // strlen(" (deleted)") == 10
            const QString key = filelock::pathKey(target, false);
            if (!filelock::matchesPath(key, match, recursive, false))
                continue;
            ++hits;
            if (firstPath.isEmpty())
                firstPath = target;
        }
        if (hits == 0)
            continue;

        filelock::Holder holder;
        holder.pid = pid;
        holder.name = readComm(pid);
        holder.type = QStringLiteral("进程");
        holder.hitCount = hits;
        holder.hitPath = hits > 1 ? QStringLiteral("%1 个文件").arg(hits) : firstPath;
        out.append(holder);
    }

    if (unreadable > 0) {
        *error = QStringLiteral("有 %1 个进程的 fd 目录读不到（权限不足），"
                                "结果可能不完整").arg(unreadable);
    }
    return out;
}

#endif // Q_OS_LINUX

#if defined(Q_OS_MACOS)

/// 解析 lsof -F pcn 的输出。
///
/// -F 是给脚本用的字段格式：p=pid、c=命令名、n=文件路径，每条记录以 p 开头。
/// 不用默认的表格输出，是因为带空格的路径/命令名会把列挤歪，解析结果不可靠。
/// 说明：本分支调用系统自带 lsof，未写任何 AppKit/Carbon 私有 API；
/// 但当前开发环境只有 Windows，此分支**未在 macOS 实机验证**，
/// 失败时通过 error 返回 lsof 的原始错误，不静默假装成功。
QVector<filelock::Holder> parseLsofOutput(const QString &text,
                                          const filelock::MatchSet &match,
                                          bool recursive, quint64 selfPid)
{
    QVector<filelock::Holder> out;
    quint64 pid = 0;
    QString command;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        if (line.isEmpty())
            continue;
        const QChar tag = line.at(0);
        const QString value = line.mid(1);
        if (tag == QLatin1Char('p')) {
            bool ok = false;
            pid = value.toULongLong(&ok);
            if (!ok)
                pid = 0;
            command.clear();
        } else if (tag == QLatin1Char('c')) {
            command = value;
        } else if (tag == QLatin1Char('n')) {
            if (pid == 0 || pid == selfPid)
                continue;
            const QString key = filelock::pathKey(value, false);
            if (!filelock::matchesPath(key, match, recursive, false))
                continue;
            filelock::Holder holder;
            holder.pid = pid;
            holder.name = command;
            holder.type = QStringLiteral("进程");
            holder.hitPath = value;
            out.append(holder);
        }
    }
    return out;
}

/// macOS 检测：每个输入路径单独调一次 lsof。
/// 目录用 +D（递归）/ +d（仅本层）对应「递归查询」复选框，文件用 -- 传路径。
/// 按输入而不是按展开后的 512 个文件调用，是为了不把参数表撑爆、也不起几百次进程。
QVector<filelock::Holder> scanLsof(const QStringList &inputs,
                                   const filelock::Targets &targets,
                                   bool recursive, quint64 selfPid, QString *error)
{
    const filelock::MatchSet match = filelock::buildMatchSet(targets, false);
    QVector<filelock::Holder> out;
    QStringList failures;
    int calls = 0;

    for (const QString &input : inputs) {
        const QFileInfo info(input);
        QStringList args{QStringLiteral("-nP"), QStringLiteral("-F"),
                         QStringLiteral("pcn")};
        if (info.isDir()) {
            args << (recursive ? QStringLiteral("+D") : QStringLiteral("+d")) << input;
        } else {
            args << QStringLiteral("--")
                 << (info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                       : info.canonicalFilePath());
        }

        QProcess process;
        process.start(QStringLiteral("lsof"), args);
        if (!process.waitForFinished(10000)) {
            failures.append(QStringLiteral("%1（%2）").arg(input, process.errorString()));
            continue;
        }
        ++calls;
        out.append(parseLsofOutput(
            QString::fromLocal8Bit(process.readAllStandardOutput()),
            match, recursive, selfPid));
        const QString stderrText =
            QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (!stderrText.isEmpty() && process.exitCode() != 0)
            failures.append(QStringLiteral("%1（%2）").arg(input, stderrText));
    }

    if (calls == 0 && !failures.isEmpty())
        *error = QStringLiteral("调用 lsof 失败：%1").arg(failures.first());
    return out;
}

#endif // Q_OS_MACOS

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)

/// POSIX 结束进程：只发 SIGTERM，给进程保存数据 / 清理的机会。
/// 刻意不发 SIGKILL —— 那会直接丢未保存数据，不该由工具替用户决定；
/// 若进程不响应，用户可以改用系统自带的强制退出。
filelock::ActionOutcome terminatePosix(const QVector<quint64> &pids, quint64 selfPid)
{
    filelock::ActionOutcome outcome;
    for (quint64 pid : pids) {
        if (pid <= 1 || pid == selfPid || pid > 0xFFFFFFFFull) {
            ++outcome.failed;
            if (outcome.error.isEmpty())
                outcome.error = QStringLiteral("系统进程与工具自身不允许结束");
            continue;
        }
        if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0) {
            outcome.succeededPids.append(pid);
        } else {
            ++outcome.failed;
            if (outcome.error.isEmpty())
                outcome.error = QStringLiteral("结束进程 %1 失败：%2")
                                    .arg(pid)
                                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
    }
    return outcome;
}

#endif // Q_OS_LINUX || Q_OS_MACOS

} // namespace

bool canDetect()
{
#if defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_LINUX)
    return true;
#elif defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

bool canReleaseHandles()
{
#if defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_LINUX)
    return false;
#elif defined(Q_OS_MACOS)
    return false;
#else
    return false;
#endif
}

bool canTerminate()
{
#if defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_LINUX)
    return true;
#elif defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

QString unsupportedDetectReason()
{
#if defined(Q_OS_WIN)
    return QString();
#elif defined(Q_OS_LINUX)
    return QString();
#elif defined(Q_OS_MACOS)
    return QString();
#else
    return QStringLiteral("当前平台不支持检测文件占用");
#endif
}

QString unsupportedReleaseReason()
{
#if defined(Q_OS_WIN)
    return QString();
#elif defined(Q_OS_LINUX)
    return QStringLiteral("当前平台不支持：Linux 没有普通用户可用的"
                          "「关闭其他进程文件句柄」接口，请改用「强制结束」");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("当前平台不支持：macOS 未开放关闭其他进程文件描述符的"
                          "接口，请改用「强制结束」");
#else
    return QStringLiteral("当前平台不支持解除文件占用");
#endif
}

QString unsupportedTerminateReason()
{
#if defined(Q_OS_WIN)
    return QString();
#elif defined(Q_OS_LINUX)
    return QString();
#elif defined(Q_OS_MACOS)
    return QString();
#else
    return QStringLiteral("当前平台不支持结束其他进程");
#endif
}

filelock::ScanOutcome scan(const QStringList &inputs, bool recursive, quint64 selfPid)
{
    filelock::ScanOutcome outcome;
    const filelock::Targets targets =
        filelock::expandTargets(inputs, recursive, kCaseInsensitive);
    if (!targets.error.isEmpty()) {
        outcome.error = targets.error;
        return outcome;
    }
    outcome.scannedFiles = targets.files.size();
    outcome.truncated = targets.truncated;

#if defined(Q_OS_WIN)
    outcome.holders = scanWindows(targets, selfPid, &outcome.error);
#elif defined(Q_OS_LINUX)
    outcome.holders = scanProcFs(targets, recursive, selfPid, &outcome.error);
#elif defined(Q_OS_MACOS)
    outcome.holders = scanLsof(inputs, targets, recursive, selfPid, &outcome.error);
#else
    Q_UNUSED(inputs)
    Q_UNUSED(recursive)
    Q_UNUSED(selfPid)
    outcome.error = unsupportedDetectReason();
#endif

    outcome.holders = filelock::mergeHolders(outcome.holders);
    return outcome;
}

filelock::ActionOutcome release(const QStringList &inputs, bool recursive,
                                const QVector<quint64> &pids, quint64 selfPid)
{
    filelock::ActionOutcome outcome;
#if defined(Q_OS_WIN)
    const filelock::Targets targets =
        filelock::expandTargets(inputs, recursive, true);
    if (!targets.error.isEmpty()) {
        outcome.error = targets.error;
        return outcome;
    }
    outcome = closeRemoteHandles(targets, pids, selfPid, recursive);
#elif defined(Q_OS_LINUX)
    Q_UNUSED(inputs)
    Q_UNUSED(recursive)
    Q_UNUSED(pids)
    Q_UNUSED(selfPid)
    outcome.error = unsupportedReleaseReason();
#elif defined(Q_OS_MACOS)
    Q_UNUSED(inputs)
    Q_UNUSED(recursive)
    Q_UNUSED(pids)
    Q_UNUSED(selfPid)
    outcome.error = unsupportedReleaseReason();
#else
    Q_UNUSED(inputs)
    Q_UNUSED(recursive)
    Q_UNUSED(pids)
    Q_UNUSED(selfPid)
    outcome.error = unsupportedReleaseReason();
#endif
    return outcome;
}

filelock::ActionOutcome terminate(const QVector<quint64> &pids, quint64 selfPid)
{
    filelock::ActionOutcome outcome;
#if defined(Q_OS_WIN)
    for (quint64 pid : pids) {
        // pid 0 = System Idle、4 = System（内核），结束它们等于搞挂系统；
        // selfPid 则是不让自己把自己杀掉。
        if (pid == 0 || pid == 4 || pid == selfPid || pid > 0xFFFFFFFFull) {
            ++outcome.failed;
            if (outcome.error.isEmpty())
                outcome.error = QStringLiteral("系统进程与工具自身不允许结束");
            continue;
        }
        HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (!handle) {
            ++outcome.failed;
            if (outcome.error.isEmpty())
                outcome.error = winErrorMessage(GetLastError());
            continue;
        }
        if (TerminateProcess(handle, 1))
            outcome.succeededPids.append(pid);
        else {
            ++outcome.failed;
            if (outcome.error.isEmpty())
                outcome.error = winErrorMessage(GetLastError());
        }
        CloseHandle(handle);
    }
#elif defined(Q_OS_LINUX)
    outcome = terminatePosix(pids, selfPid);
#elif defined(Q_OS_MACOS)
    outcome = terminatePosix(pids, selfPid);
#else
    Q_UNUSED(pids)
    Q_UNUSED(selfPid)
    outcome.error = unsupportedTerminateReason();
#endif
    return outcome;
}

} // namespace fileprobe

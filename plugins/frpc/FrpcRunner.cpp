#include "FrpcRunner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace frpc {
namespace {

/// 日志缓冲区上限。frpc 的输出是「登录一次 + 每条转发建立/断开时一行」，
/// 4000 行足够覆盖几天的使用；再长也没有人会往上翻。
constexpr int kMaxBufferedLines = 4000;

/// POSIX 上 SIGTERM 之后等多久才 SIGKILL。frpc 收到 SIGTERM 会立刻退出，
/// 4 秒是给「机器正好卡一下」留的余量。
constexpr int kForceKillDelayMs = 4000;

/// 可执行文件候选目录：程序所在目录、插件目录（有人把 frpc 丢这儿）、
/// 以及允许用户自己放到 AppLocalData/frpc/bin 的固定位置。
QStringList candidateDirs()
{
    QStringList dirs;

    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        dirs << appDir;
        dirs << appDir + QStringLiteral("/plugins");
        dirs << appDir + QStringLiteral("/../frpc/bin");
    }

    const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!local.isEmpty())
        dirs << local + QStringLiteral("/frpc/bin");

    return dirs;
}

/// 判断一个路径是不是「可以直接执行的文件」。
///
/// Windows 上没有执行位，只能看文件是否存在且后缀是 .exe。刻意**不接受** .bat / .cmd：
/// QProcess 在 Windows 上不能直接 CreateProcess 一个批处理（必须先经 cmd.exe /c），
/// 放进来只会在启动时失败，错误信息还很难懂。真要用脚本包装 frpc，请先包成 .exe。
/// POSIX 上要求真的有执行位，否则会失败在「权限不够」而不是「找不到」。
bool isRunnableFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile())
        return false;
#if defined(Q_OS_WIN)
    return info.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) == 0;
#else
    return info.isExecutable();
#endif
}

} // namespace

namespace paths {

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/frpc/conf");
}

QString configPathFor(const QString &profileId, frpctext::Format format)
{
    return configDir() + QLatin1Char('/') + profileId + QLatin1Char('.')
        + (format == frpctext::Format::Ini ? QStringLiteral("ini") : QStringLiteral("toml"));
}

QString executableName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("frpc.exe");
#else
    return QStringLiteral("frpc");
#endif
}

} // namespace paths

QString locate(const QString &preferred)
{
    if (!preferred.trimmed().isEmpty()) {
        // 用户手填的路径优先级最高。填错了就直接回报空 —— 不要「猜用户想指哪个」，
        // 否则用户会以为自己填的生效了，实际跑的是别处的旧版本 frpc。
        return isRunnableFile(preferred.trimmed()) ? QFileInfo(preferred.trimmed()).absoluteFilePath()
                                                   : QString();
    }

    // PATH：装了 frpc 的机器大多能在这里命中。findExecutable 在 Windows 上会自己
    // 补 PATHEXT，所以两种写法都试一下（不同 Qt 版本行为略有差异）。
    const QStringList names = {paths::executableName(), QStringLiteral("frpc")};
    for (const QString &name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }

    for (const QString &dir : candidateDirs()) {
        const QString candidate = QDir(dir).filePath(paths::executableName());
        if (isRunnableFile(candidate))
            return QFileInfo(candidate).absoluteFilePath();
    }

    return QString();
}

QString locateHint()
{
    return QStringLiteral(
        "没找到 frpc。到 github.com/fatedier/frp/releases 下载对应平台的压缩包，"
        "把 frpc（Windows 是 frpc.exe）放到本工具安装目录，或填它的完整路径。");
}

Service *Service::instance()
{
    // 函数内 static：不挂在 qApp 名下。qApp 若持有它，DLL 卸载时会去 delete 一段
    // 已 unmap 的 vtable（screenrec 的 RecordService 踩过同一个坑）。
    // 首次调用发生在面板构造时，一定在主线程。
    static Service *service = new Service();
    return service;
}

Service::Service()
    : QObject(nullptr)
{
    m_process = new QProcess(this);
    // 合并两个通道：frpc 的错误也走 stderr，分开读会让日志顺序错乱（"error" 排在
    // 导致它的那行上面）。
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process, &QProcess::readyRead, this, [this] {
        consume(m_process->readAll());
    });

    // 半行可能卡在缓冲区里：进程退出时把剩下的补上，否则最后一条错误会看不到。
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        const QByteArray rest = m_process->readAll();
        if (!rest.isEmpty())
            consume(rest);
        handleFinished(exitCode, status == QProcess::CrashExit ? 1 : 0);
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;   // 崩溃/读写错误都会跟一条 finished，不重复报
        m_status.phase = Phase::Idle;
        m_status.lastError = QStringLiteral("启动失败：%1").arg(m_process->errorString());
        appendLine(QStringLiteral("[错误] ") + m_status.lastError);
        emit stateChanged();
        emit outputArrived();
    });

    m_forceKillTimer = new QTimer(this);
    m_forceKillTimer->setSingleShot(true);
    m_forceKillTimer->setInterval(kForceKillDelayMs);
    connect(m_forceKillTimer, &QTimer::timeout, this, [this] {
        if (m_process->state() == QProcess::NotRunning)
            return;
        appendLine(QStringLiteral("[提示] 4 秒内没有退出，强制结束 frpc"));
        m_process->kill();
    });

    // 退出工具箱时把 frpc 一起停掉。
    //
    // 为什么不留着跑：本工具定位是「本地小工具」，用户关掉窗口就认为一切都停了；
    // 留下一个后台网络服务，既看不见也关不掉（下次启动还会因为「端口已占用」
    // 而困惑）。如果哪天需要「关工具箱也保持穿透」，正确做法是让用户用系统服务
    // 或独立部署 frpc，而不是让一个 GUI 工具偷偷养进程。
    if (qApp) {
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            if (m_process->state() != QProcess::NotRunning) {
                // 退出路径上不能等：直接强杀，进程本来也活不过宿主。
                m_process->kill();
                m_process->waitForFinished(800);
            }
        });
    }
}

Service::~Service() = default;

bool Service::runsProfile(const QString &profileId) const
{
    return (m_status.phase == Phase::Running || m_status.phase == Phase::Stopping)
        && m_status.profileId == profileId;
}

Outcome Service::start(const QString &profileId, const QString &program,
                       const QString &configText, frpctext::Format format)
{
    Outcome outcome;

    if (m_status.phase != Phase::Idle) {
        outcome.error = QStringLiteral("已经有一份配置在运行，先停止再启动");
        return outcome;
    }
    if (program.trimmed().isEmpty()) {
        outcome.error = QStringLiteral("还没设置 frpc 可执行文件");
        return outcome;
    }
    if (!isRunnableFile(program)) {
        outcome.error = QStringLiteral("frpc 可执行文件不存在或不可执行：%1").arg(program);
        return outcome;
    }

    // 配置必须落盘：frpc 只接受 `-c <文件>`，没有从标准输入读配置的入口。
    // 用 QSaveFile 原子替换，避免「正在启动时面板自动保存」把文件写成半截，
    // 让 frpc 读到语法错误。
    QDir().mkpath(paths::configDir());
    const QString configPath = paths::configPathFor(profileId, format);
    {
        QSaveFile file(configPath);
        if (!file.open(QIODevice::WriteOnly)) {
            outcome.error = QStringLiteral("无法写入配置文件：%1").arg(file.errorString());
            return outcome;
        }
        file.write(configText.toUtf8());
        if (!file.commit()) {
            outcome.error = QStringLiteral("配置文件写入失败：%1").arg(file.errorString());
            return outcome;
        }
    }

    // 用程序所在目录做工作目录：frpc 会按相对路径找证书 / 插件（服务端才有），
    // 用工具箱自己的目录会让这些相对路径落到意外的地方。
    m_process->setWorkingDirectory(QFileInfo(program).absolutePath());
    m_process->setProgram(program);
    m_process->setArguments({QStringLiteral("-c"), configPath});
    m_process->start();

    if (!m_process->waitForStarted(5000)) {
        outcome.error = QStringLiteral("frpc 启动失败：%1").arg(m_process->errorString());
        m_status.phase = Phase::Idle;
        m_status.lastError = outcome.error;
        appendLine(QStringLiteral("[错误] ") + outcome.error);
        emit stateChanged();
        emit outputArrived();
        return outcome;
    }

    m_status.phase = Phase::Running;
    m_status.profileId = profileId;
    m_status.program = program;
    m_status.pid = m_process->processId();
    m_status.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_status.lastError.clear();
    m_status.lastExitCode = 0;
    m_status.lastCrashed = false;

    // 分隔线：日志是跨次累积的，没有分隔线时用户分不清哪段是这次启动的输出。
    appendLine(QStringLiteral("===== %1 启动 frpc（配置 %2）=====")
                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                        QFileInfo(configPath).fileName()));

    outcome.ok = true;
    emit stateChanged();
    emit outputArrived();
    return outcome;
}

Outcome Service::stop()
{
    Outcome outcome;

    if (m_process->state() == QProcess::NotRunning) {
        if (m_status.phase != Phase::Idle) {
            m_status.phase = Phase::Idle;
            emit stateChanged();
        }
        outcome.error = QStringLiteral("frpc 没有在运行");
        return outcome;
    }

    m_status.phase = Phase::Stopping;
    emit stateChanged();

#if defined(Q_OS_WIN)
    // Windows 上 QProcess::terminate() 是往目标进程的**窗口**投 WM_CLOSE。
    // frpc 是控制台程序，没有消息循环，投了也没人接 —— 只能直接 TerminateProcess。
    m_process->kill();
#else
    m_process->terminate();   // SIGTERM
    m_forceKillTimer->start();
#endif

    outcome.ok = true;
    return outcome;
}

void Service::handleFinished(int exitCode, int exitStatus)
{
    m_forceKillTimer->stop();

    const bool wasRunning = m_status.phase != Phase::Idle;
    m_status.phase = Phase::Idle;
    m_status.lastExitCode = exitCode;
    // exitStatus != 0 表示 CrashExit。用户主动停止时也算「非正常退出」，
    // 但面板会用「最后是我点的停止」来判断要不要告警，不单看这个标志。
    m_status.lastCrashed = exitStatus != 0;

    if (wasRunning) {
        appendLine(QStringLiteral("===== frpc 已退出（退出码 %1）=====").arg(exitCode));
        if (exitStatus != 0)
            m_status.lastError = QStringLiteral("frpc 异常退出（退出码 %1）").arg(exitCode);
    }

    emit stateChanged();
    emit outputArrived();
}

void Service::consume(const QByteArray &raw)
{
    if (raw.isEmpty())
        return;

    m_partial += raw;

    // frpc 在 Windows 上输出 CRLF，Linux 上是 LF；统一按 \n 切，尾部 \r 单独去掉，
    // 否则面板里每行末尾会多一个不可见字符（复制出来就知道）。
    bool appended = false;
    while (true) {
        const int newline = m_partial.indexOf('\n');
        if (newline < 0)
            break;

        QString line = QString::fromUtf8(m_partial.left(newline));
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        m_partial.remove(0, newline + 1);
        appendLine(line);
        appended = true;
    }

    // 只有真的凑成整行才通知面板：读到半行时提醒也没用（面板拿不到新行），
    // 反而会让 pumpLog 在长日志里被高频空转。剩下的半行等下一次 chunk 或进程退出时补上。
    if (appended)
        emit outputArrived();
}

void Service::appendLine(const QString &line)
{
    m_lines.append(line);
    ++m_totalLines;
    if (m_lines.size() > kMaxBufferedLines)
        m_lines.remove(0, m_lines.size() - kMaxBufferedLines);
}

Service::LogChunk Service::since(qint64 index) const
{
    LogChunk chunk;

    const qint64 firstAvailable = m_totalLines - m_lines.size();
    if (index < 0)
        index = 0;

    if (index < firstAvailable) {
        // 面板太久没取（切页期间日志暴涨），它手上那些行已经滚掉了。
        // 与其留下一个断层，不如让面板整块重画：lostEarlier 为 true 时调用方会清空。
        chunk.lostEarlier = true;
        index = firstAvailable;
    }
    if (index > m_totalLines)
        index = m_totalLines;

    const int begin = static_cast<int>(index - firstAvailable);
    chunk.lines = m_lines.mid(begin);
    chunk.next = m_totalLines;
    return chunk;
}

QStringList Service::tail(int maxLines) const
{
    if (maxLines <= 0)
        return {};
    const int begin = qMax(0, m_lines.size() - maxLines);
    return m_lines.mid(begin);
}

void Service::clearLog()
{
    m_lines.clear();
    m_totalLines = 0;
    // 注意：不清 m_partial。半行是 frpc 正在写的字节，丢掉会让下一行的前半句消失。
}

} // namespace frpc

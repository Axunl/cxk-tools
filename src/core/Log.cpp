#include "Log.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>
#include <cstdio>

#if defined(Q_OS_WIN)
#include <QtCore/qt_windows.h>
#endif

namespace {

/// 日志放在 AppLocalDataLocation 下（Windows 是不可漫游的本地数据目录，
/// 避免企业环境下漫游配置文件把日志同步到服务器）。
QString logFilePathImpl()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                        + QStringLiteral("/logs");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/app.log");
}

/// 级别标签固定宽度，方便肉眼对齐、也方便 grep。
const char *levelTag(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO ";
    case QtWarningMsg:  return "WARN ";
    case QtCriticalMsg: return "ERROR";
    case QtFatalMsg:    return "FATAL";
    }
    return "INFO ";
}

/// 全局消息处理器。
///
/// 注意两点：
///  1. 加锁 —— 插件可能在非 GUI 线程里 qWarning；
///  2. 每条都重新 open/append/close，不做常驻句柄。日志量极小，换来的是
///     进程被强杀时也不丢最后几行（崩溃排查恰恰最需要这几行）。
void handler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context)

    static QMutex mutex;
    QMutexLocker locker(&mutex);

    const QString line = QStringLiteral("[%1] [%2] %3")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                                  QLatin1String(levelTag(type)), msg);

    // 先写 stderr：开发期直接跑 exe 时能立刻看到，不依赖文件。
    // 必须按 UTF-8 写：qPrintable() 是 toLocal8Bit()（中文 Windows 下是 GBK 字节），
    // 而 IDE 的运行控制台按 UTF-8 解码，GBK 字节会整屏变 "???" 乱码（已踩过）。
    const QByteArray utf8 = line.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);

    QFile file(logFilePathImpl());
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);   // 日志里有中文，不能用本地编码
        out << line << '\n';
    }

    // QtFatalMsg 默认语义就是中止进程，这里保持
    if (type == QtFatalMsg)
        std::abort();
}

} // namespace

QString Log::filePath()
{
    return logFilePathImpl();
}

void Log::init()
{
#if defined(Q_OS_WIN)
    // stderr 按 UTF-8 输出后，真控制台（cmd / PowerShell）默认 GBK 代码页照样会乱码，
    // 有控制台时把输出代码页切到 UTF-8。双击启动的 GUI 进程没有控制台，此调用无害。
    if (GetConsoleWindow() != nullptr)
        SetConsoleOutputCP(CP_UTF8);
#endif
    qInstallMessageHandler(handler);
    qInfo() << "---- cxk-tools started ----";
}

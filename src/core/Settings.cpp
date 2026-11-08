#include "Settings.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace {

// 配置键。集中在这里定义，避免各处拼字符串拼错导致「设置存了但读不回来」。
//
// 组名用 "settings" 而不是更直觉的 "general"：INI 的 [General] 是保留段名，
// QSettings 会把同名组转义成 "[%General]"（大小写不敏感），手工编辑时非常费解。
constexpr auto kGroupGeneral = "settings";
constexpr auto kKeyHotkey = "hotkey";
constexpr auto kKeyAutostart = "start_with_system";
constexpr auto kKeyLastTool = "last_tool";
constexpr auto kGroupUsage = "usage";

#ifdef Q_OS_WIN
constexpr auto kRunKeyPath = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto kRunValueName = "CXKTools";
#endif

/// INI 路径。
///
/// 注意：QSettings 默认构造在 Windows 上走注册表，setPath() 对它无效；
/// 想要真正落到 ini 文件，必须用 QSettings(filePath, IniFormat) 构造。
/// 这里顺手 mkpath，避免首次启动时写盘失败。
QString iniFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/cxk-tools.ini");
}

} // namespace

Settings &Settings::instance()
{
    static Settings s;
    return s;
}

Settings::Settings()
    : m_settings(iniFilePath(), QSettings::IniFormat)
{
    // 关掉 fallback：否则未写入的键会去系统注册表里找同名值，
    // 出现「配置文件里明明没有、读出来却有值」的灵异现象。
    m_settings.setFallbacksEnabled(false);
}

QString Settings::configFilePath() const
{
    return m_settings.fileName();
}

void Settings::sync()
{
    m_settings.sync();
}

QKeySequence Settings::defaultHotkey()
{
    return QKeySequence(QStringLiteral("Ctrl+Alt+Space"));
}

QKeySequence Settings::hotkey() const
{
    const QString s = m_settings.value(QLatin1String(kGroupGeneral) + QLatin1Char('/')
                                       + QLatin1String(kKeyHotkey))
                          .toString();
    if (s.isEmpty())
        return defaultHotkey();

    // 用户可能手工编辑 ini 写坏了，解析失败就回退默认值而不是给个空热键
    const QKeySequence seq(s);
    return seq.isEmpty() ? defaultHotkey() : seq;
}

void Settings::setHotkey(const QKeySequence &seq)
{
    // 存 PortableText（"Ctrl+Alt+Space"），跨平台可读且能手工编辑；
    // NativeText 在 Windows 上是 "Ctrl+Alt+Space"，在 macOS 上会变成 ⌃⌥Space，
    // 存进 ini 反而不好维护。
    m_settings.setValue(QLatin1String(kGroupGeneral) + QLatin1Char('/') + QLatin1String(kKeyHotkey),
                        seq.toString(QKeySequence::PortableText));
    m_settings.sync();
}

bool Settings::startWithSystem() const
{
    return m_settings.value(QLatin1String(kGroupGeneral) + QLatin1Char('/')
                                + QLatin1String(kKeyAutostart),
                            false)
        .toBool();
}

void Settings::setStartWithSystem(bool on)
{
    m_settings.setValue(QLatin1String(kGroupGeneral) + QLatin1Char('/') + QLatin1String(kKeyAutostart), on);
    m_settings.sync();

    // 配置先落盘，再动系统项：即使系统项设置失败，用户下次打开设置页看到的
    // 勾选状态也与他的操作一致
    applyAutostart(on);
}

QString Settings::lastToolId() const
{
    return m_settings.value(QLatin1String(kGroupGeneral) + QLatin1Char('/') + QLatin1String(kKeyLastTool))
        .toString();
}

void Settings::setLastToolId(const QString &id)
{
    // 故意不 sync：切换工具很频繁，等进程退出或下次 sync 时一并落盘即可
    m_settings.setValue(QLatin1String(kGroupGeneral) + QLatin1Char('/') + QLatin1String(kKeyLastTool), id);
}

int Settings::usage(const QString &toolId) const
{
    return m_settings.value(QLatin1String(kGroupUsage) + QLatin1Char('/') + toolId, 0).toInt();
}

void Settings::bumpUsage(const QString &toolId)
{
    m_settings.setValue(QLatin1String(kGroupUsage) + QLatin1Char('/') + toolId, usage(toolId) + 1);
    m_settings.sync();
}

QString Settings::autostartCommand()
{
    // 带引号：安装路径可能含空格（Program Files 就是）
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return QStringLiteral("\"%1\" --autostart").arg(exe);
}

void Settings::applyAutostart(bool on)
{
#if defined(Q_OS_WIN)
    QSettings runKey(QLatin1String(kRunKeyPath), QSettings::NativeFormat);
    if (on)
        runKey.setValue(QLatin1String(kRunValueName), autostartCommand());
    else
        runKey.remove(QLatin1String(kRunValueName));
    runKey.sync();
#elif defined(Q_OS_LINUX)
    // XDG autostart：~/.config/autostart/cxk-tools.desktop
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                        + QStringLiteral("/autostart");
    const QString file = dir + QStringLiteral("/cxk-tools.desktop");

    if (!on) {
        QFile::remove(file);
        return;
    }

    QDir().mkpath(dir);
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "Settings: 写入自启动文件失败:" << file;
        return;
    }

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=CXK 工具箱\n"
        << "Comment=离线桌面工具箱\n"
        << "Exec=\"" << QCoreApplication::applicationFilePath() << "\" --autostart\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n"
        << "Hidden=false\n";
#else
    // macOS 需要写 LaunchAgent plist（~/Library/LaunchAgents），尚未实现。
    // 明确记录并返回，不做「看起来成功了」的空操作。
    qWarning() << "Settings: 当前平台尚未实现开机自启（macOS 待补 LaunchAgent），已忽略:" << on;
#endif
}

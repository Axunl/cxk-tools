#include "Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>

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
constexpr auto kKeyTheme = "theme";
constexpr auto kGroupUsage = "usage";
/// 最近使用：recent/<toolId> = UTC ISO8601 时间戳。
/// 单独成组而不是塞进 usage：usage 是 int 计数，改存复合值会让老用户的频次作废。
constexpr auto kGroupRecent = "recent";
/// 窗口几何：ui/<name>/geometry = QWidget::saveGeometry() 的字节流。
constexpr auto kGroupUi = "ui";
/// 更新检查（见 UpdateChecker）：update/auto_check、update/last_check、update/skipped_version。
constexpr auto kGroupUpdate = "update";
constexpr auto kKeyAutoCheck = "auto_check";
constexpr auto kKeyLastCheck = "last_check";
constexpr auto kKeySkippedVersion = "skipped_version";

#ifdef Q_OS_WIN
constexpr auto kRunKeyPath = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto kRunValueName = "CXKTools";
#endif

/// 解析一条最近使用的时间戳。
/// 主格式带毫秒（同一秒内连开两个工具时，秒级精度会让"最新"退化成按 id 排序），
/// 再兜底解析秒级 ISODate，兼容手工编辑过的旧值。
QDateTime parseRecentTimestamp(const QString &raw)
{
    QDateTime dt = QDateTime::fromString(raw, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(raw, Qt::ISODate);
    return dt;
}

/// 几何键名：ui/<name>/geometry。
/// name 里的 '/' 统一换成 '_'——插件 id 若带斜杠会把 QSettings 的组层级打断，
/// 之后按原 name 读回来就变成另一个键了。
QString geometryKey(const QString &name)
{
    QString safe = name;
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QLatin1String(kGroupUi) + QLatin1Char('/') + safe + QStringLiteral("/geometry");
}

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

QString Settings::themeMode() const
{
    // 默认 "system"：深色系统上首次启动就应该是深色，而不是"白得晃眼"
    return m_settings.value(QLatin1String(kGroupGeneral) + QLatin1Char('/') + QLatin1String(kKeyTheme),
                            QStringLiteral("system"))
        .toString();
}

void Settings::setThemeMode(const QString &mode)
{
    m_settings.setValue(QLatin1String(kGroupGeneral) + QLatin1Char('/') + QLatin1String(kKeyTheme), mode);
    m_settings.sync();
}

QStringList Settings::recentToolIds(int limit) const
{
    struct Item
    {
        qint64 ts;
        QString id;
    };

    // 一次 allKeys() 扫描而不是递归 beginGroup：键很少（<= 工具数），
    // 而且这样天然跳过其它组，不用管组名边界。
    const QString prefix = QLatin1String(kGroupRecent) + QLatin1Char('/');
    QList<Item> items;
    const QStringList keys = m_settings.allKeys();
    items.reserve(keys.size());

    for (const QString &key : keys) {
        if (!key.startsWith(prefix))
            continue;
        const QString id = key.mid(prefix.size());
        if (id.isEmpty())
            continue;

        // 解析失败（用户手工编辑写坏）就跳过这一条，不能让整组消失
        const QDateTime dt = parseRecentTimestamp(m_settings.value(key).toString());
        if (!dt.isValid())
            continue;
        // 用毫秒而不是秒：同一秒内连开两个工具时，秒级精度会让"最新"退化成按 id 排序
        items.append({dt.toMSecsSinceEpoch(), id});
    }

    // 按 epoch 倒序：不要直接字符串比较，跨时区写进来的老值会错位。
    // 同一秒写入时用 id 兜底，保证顺序稳定可复现。
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.ts != b.ts)
            return a.ts > b.ts;
        return a.id < b.id;
    });

    QStringList out;
    for (const Item &item : items) {
        if (limit > 0 && out.size() >= limit)
            break;
        out.append(item.id);
    }
    return out;
}

void Settings::touchRecent(const QString &toolId)
{
    const QString id = toolId.trimmed();
    if (id.isEmpty())
        return;

    m_settings.setValue(QLatin1String(kGroupRecent) + QLatin1Char('/') + id,
                        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    // 裁剪到上限：只保留最新的 kRecentLimit 条，其余键直接删掉，
    // 否则用户每试一个插件 ini 就多一行，永久增长。
    const QStringList all = recentToolIds(0);
    for (int i = kRecentLimit; i < all.size(); ++i) {
        m_settings.remove(QLatin1String(kGroupRecent) + QLatin1Char('/') + all.at(i));
    }
    m_settings.sync();
}

QByteArray Settings::windowGeometry(const QString &name) const
{
    return m_settings.value(geometryKey(name)).toByteArray();
}

void Settings::setWindowGeometry(const QString &name, const QByteArray &geo)
{
    // 故意不 sync：几何只在关窗/进程退出时写，跟着退出前的那次 sync 一起落盘即可
    m_settings.setValue(geometryKey(name), geo);
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

bool Settings::autoCheckUpdate() const
{
    // 默认开：这是个桌面工具，用户不会主动点「检查更新」，默认关等于永远不更新
    return m_settings.value(QLatin1String(kGroupUpdate) + QLatin1Char('/') + QLatin1String(kKeyAutoCheck),
                            true)
        .toBool();
}

void Settings::setAutoCheckUpdate(bool on)
{
    m_settings.setValue(QLatin1String(kGroupUpdate) + QLatin1Char('/') + QLatin1String(kKeyAutoCheck), on);
    m_settings.sync();
}

QDateTime Settings::lastUpdateCheck() const
{
    const QString raw = m_settings
                            .value(QLatin1String(kGroupUpdate) + QLatin1Char('/')
                                   + QLatin1String(kKeyLastCheck))
                            .toString();
    if (raw.isEmpty())
        return QDateTime();

    QDateTime dt = QDateTime::fromString(raw, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(raw, Qt::ISODate);
    if (!dt.isValid())
        return QDateTime();

    // 统一换算到 UTC：手工编辑过 ini 时可能写的是本地时间，不换算会让节流算错好几小时
    return dt.toUTC();
}

void Settings::setLastUpdateCheck(const QDateTime &when)
{
    m_settings.setValue(QLatin1String(kGroupUpdate) + QLatin1Char('/') + QLatin1String(kKeyLastCheck),
                        when.toUTC().toString(Qt::ISODate));
    m_settings.sync();
}

QString Settings::skippedVersion() const
{
    return m_settings.value(QLatin1String(kGroupUpdate) + QLatin1Char('/')
                            + QLatin1String(kKeySkippedVersion))
        .toString();
}

void Settings::setSkippedVersion(const QString &version)
{
    m_settings.setValue(QLatin1String(kGroupUpdate) + QLatin1Char('/') + QLatin1String(kKeySkippedVersion),
                        version);
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

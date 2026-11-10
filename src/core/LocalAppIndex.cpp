#include "LocalAppIndex.h"

#include "Pinyin.h"
#include "Settings.h"

#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <algorithm>

namespace {

/// 索引上限：防止某些被塞满菜单的机器把启动拖长（正常机器远低于此值）。
constexpr int kMaxEntries = 1200;

/// 明显不是「要打开的应用」的入口：卸载器、说明文档、网址捷径。
/// 只做名称粗筛，宁可漏掉几个也不要把用户菜单塞满垃圾。
const QStringList &junkKeywords()
{
    static const QStringList keywords = {
        QStringLiteral("uninstall"), QStringLiteral("卸载"),
        QStringLiteral("readme"), QStringLiteral("read me"),
        QStringLiteral("help"), QStringLiteral("帮助"),
        QStringLiteral("documentation"), QStringLiteral("manual"),
        QStringLiteral("license"), QStringLiteral("changelog"),
        QStringLiteral("release notes"), QStringLiteral("官网"),
        QStringLiteral("website"), QStringLiteral("文档"),
    };
    return keywords;
}

bool looksLikeJunk(const QString &name)
{
    const QString lower = name.toCaseFolded();
    for (const QString &keyword : junkKeywords()) {
        if (lower.contains(keyword))
            return true;
    }
    return false;
}

/// 把匹配分数算在一处：得分越高越靠前，0 表示不命中。
/// 规则与 ToolRegistry 的搜索保持同一套直觉（相等 > 前缀 > 词首 > 包含）。
int matchScore(const QString &name, const QString &query)
{
    const QString lower = name.toCaseFolded();
    if (lower == query)
        return 100;
    if (lower.startsWith(query))
        return 90;

    static const QRegularExpression separators(QStringLiteral("[\\s\\-_/()\\[\\]]+"));
    const QStringList words = lower.split(separators, Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word.startsWith(query))
            return 80;
    }
    if (lower.contains(query))
        return 70;

    // 首字母缩写：vsc → Visual Studio Code。中文名也会参与（取每个词的第一个字）。
    QString initials;
    for (const QString &word : words)
        initials.append(word.at(0));
    if (!initials.isEmpty() && initials.startsWith(query))
        return 65;

    return 0;
}

/// 查询是否「纯 ASCII 字母数字」：这是走拼音/缩写比较的前提。
/// 中文查询不必算拼音（字面匹配已经足够精确），也省掉每键一次的转换开销。
bool isAsciiWord(const QString &query)
{
    if (query.isEmpty())
        return false;
    for (const QChar ch : query) {
        const ushort u = ch.unicode();
        if (!((u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z')))
            return false;
    }
    return true;
}

/// 一套「缩写 + 全拼」的命中分，0 表示不命中。
/// @param penalty 扣分：次读音整体比主读音低 2 分，主读音命中永远排在前面
int ladderScore(const QString &initials, const QString &pinyin, const QString &query, int penalty)
{
    if (!initials.isEmpty() && initials == query)
        return 68 - penalty;   // wx = 微信：整串对上，先于「微信读书」这类前缀命中
    if (!pinyin.isEmpty() && pinyin == query)
        return 66 - penalty;   // weixin = 微信
    if (!initials.isEmpty() && initials.startsWith(query))
        return 63 - penalty;   // wx → 微信读书
    if (!pinyin.isEmpty() && pinyin.startsWith(query))
        return 61 - penalty;   // weix → 微信
    if (!pinyin.isEmpty() && pinyin.contains(query))
        return 45 - penalty;   // yinyue → 网易云音乐（全拼中间命中，能搜到但排在最后）
    return 0;
}

/// 拼音/缩写命中：wx → 微信，weixin → 微信，wxdsh → 微信读书。
///
/// 分值安排在「名称字面匹配」之后（100/90/80/70/65）、「可执行文件名」之前（50）：
/// 名称里写着的字永远比读音更可信，但读懂了读音的命中比一个文件名相似更该靠前。
///
/// 主读音与次读音各比一次：yinyue 命中「网易云音乐」靠的就是次读音（乐→yue），
/// 而 kuaile 仍然走主读音，两条路互不干扰。
int aliasScore(const LocalAppIndex::Entry &entry, const QString &query)
{
    if (!isAsciiWord(query))
        return 0;

    int best = ladderScore(entry.initials, entry.pinyin, query, 0);
    if (!entry.altInitials.isEmpty() || !entry.altPinyin.isEmpty())
        best = std::max(best, ladderScore(entry.altInitials, entry.altPinyin, query, 2));
    return best;
}

#if defined(Q_OS_LINUX)

/// .desktop 只取主段 [Desktop Entry]，这里只保留首页需要的那几个键。
struct DesktopDraft
{
    QString name;
    QString exec;
    QString icon;
    bool usable = false;
};

/// 去掉 Exec 里的字段码（%f %F %u %U %i %c %k …），否则 sh 会把它们当普通参数。
QString stripFieldCodes(const QString &exec)
{
    static const QRegularExpression codes(QStringLiteral("%[fFuUdDnNickvm]"));
    QString cmd = exec;
    cmd.remove(codes);
    return cmd.simplified();
}

DesktopDraft parseDesktopFile(const QString &path)
{
    DesktopDraft draft;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return draft;

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    // 优先本地化名称：先精确区域（zh_CN），再语言（zh），最后 Name。
    const QString localeName = QLocale::system().name();
    const QString language = localeName.section(QLatin1Char('_'), 0, 0);
    const QString localizedKeys[] = {QStringLiteral("Name[%1]").arg(localeName),
                                     QStringLiteral("Name[%1]").arg(language)};

    QString type;
    QString localizedName;
    bool inMainSection = false;
    bool hidden = false;
    bool noDisplay = false;
    bool terminal = false;

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            inMainSection = (line == QLatin1String("[Desktop Entry]"));
            continue;
        }
        if (!inMainSection)
            continue;

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();

        if (key == QLatin1String("Type")) {
            type = value;
        } else if (key == QLatin1String("Name")) {
            draft.name = value;
        } else if (key == localizedKeys[0] || key == localizedKeys[1]) {
            if (localizedName.isEmpty())
                localizedName = value;
        } else if (key == QLatin1String("Exec")) {
            draft.exec = stripFieldCodes(value);
        } else if (key == QLatin1String("Icon")) {
            draft.icon = value;
        } else if (key == QLatin1String("Hidden")) {
            hidden = (value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
        } else if (key == QLatin1String("NoDisplay")) {
            noDisplay = (value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
        } else if (key == QLatin1String("Terminal")) {
            terminal = (value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0);
        }
    }

    if (!localizedName.isEmpty())
        draft.name = localizedName;

    // Terminal=true 的是命令行工具，放进图形启动器里点了也没有窗口，直接跳过
    draft.usable = (type == QLatin1String("Application")) && !hidden && !noDisplay && !terminal
                   && !draft.name.isEmpty() && !draft.exec.isEmpty();
    return draft;
}

#endif // Q_OS_LINUX

} // namespace

LocalAppIndex::LocalAppIndex(QObject *parent)
    : QObject(parent)
{
}

QString LocalAppIndex::idForPath(const QString &path)
{
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    // Windows 文件系统大小写不敏感：同一个入口用不同大小写写法不能产生两个 id
    key = key.toCaseFolded();
#endif

    // FNV-1a 64 位：实现只有几行，结果稳定，跨进程一致（不能用 qHash，
    // Qt 允许给 qHash 加随机种子，重启后 id 会变、最近使用就全失效了）。
    quint64 hash = 1469598103934665603ULL;
    for (const QChar ch : key) {
        hash ^= static_cast<quint64>(ch.unicode());
        hash *= 1099511628211ULL;
    }
    return QStringLiteral("app:") + QString::number(hash, 16);
}

bool LocalAppIndex::isAppId(const QString &id)
{
    return id.startsWith(QLatin1String("app:"));
}

void LocalAppIndex::scan()
{
    m_entries.clear();
    m_icons.clear();

#if defined(Q_OS_WIN)
    scanWindows();
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    scanMac();
#elif defined(Q_OS_LINUX)
    scanLinux();
#endif

    qInfo() << "LocalAppIndex: indexed" << m_entries.size() << "applications";
    emit changed();
}

const LocalAppIndex::Entry *LocalAppIndex::entryById(const QString &id) const
{
    // 应用数量在几百量级，线性查找比维护哈希表更省心（索引只在启动时重建）
    for (const Entry &entry : m_entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

QStringList LocalAppIndex::search(const QString &query, int limit) const
{
    const QString q = query.trimmed().toCaseFolded();
    if (q.isEmpty() || limit <= 0)
        return {};

    struct Hit
    {
        int score = 0;
        int usage = 0;
        const Entry *entry = nullptr;
    };

    QList<Hit> hits;
    for (const Entry &entry : m_entries) {
        int score = matchScore(entry.name, q);
        // 字面没命中时才比拼音/缩写：名称里真有的字比读音可信，不能被拼音盖过去
        if (score == 0)
            score = aliasScore(entry, q);
        if (score == 0) {
            // 名称没命中时再试可执行文件名（Firefox 的 desktop 名可能是 "Firefox Web Browser"）
            const QString base = QFileInfo(entry.path).completeBaseName();
            if (matchScore(base, q) > 0)
                score = 50;
        }
        if (score == 0)
            continue;

        // 用过的应用略微靠前，但不足以盖过名称匹配的差距
        const int usage = Settings::instance().usage(entry.id);
        hits.append({score + qMin(usage, 20), usage, &entry});
    }

    std::sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.usage != b.usage)
            return a.usage > b.usage;
        return a.entry->name.localeAwareCompare(b.entry->name) < 0;
    });

    QStringList ids;
    for (const Hit &hit : hits) {
        if (ids.size() >= limit)
            break;
        ids.append(hit.entry->id);
    }
    return ids;
}

QIcon LocalAppIndex::icon(const QString &id) const
{
    if (const auto it = m_icons.constFind(id); it != m_icons.constEnd())
        return it.value();

    const Entry *entry = entryById(id);
    if (!entry)
        return QIcon();

    QIcon result;
#if defined(Q_OS_LINUX)
    // .desktop 的 Icon= 通常是主题名；绝对路径也允许
    if (!entry->iconName.isEmpty()) {
        result = entry->iconName.startsWith(QLatin1Char('/'))
                     ? QIcon(entry->iconName)
                     : QIcon::fromTheme(entry->iconName);
    }
#endif
    if (result.isNull())
        result = m_provider.icon(QFileInfo(entry->path));

    m_icons.insert(id, result);
    return result;
}

bool LocalAppIndex::launch(const QString &id, QString *error) const
{
    const Entry *entry = entryById(id);
    if (!entry) {
        if (error)
            *error = QStringLiteral("找不到这个应用的启动入口，可能是刚被卸载或移动了。");
        return false;
    }

#if defined(Q_OS_LINUX)
    // 优先 gio：它按 desktop 文件走 DBus 激活，能拉起已经在后台的单实例进程。
    // 没有 gio（精简发行版）再退回 Exec 命令行。
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()) {
        if (QProcess::startDetached(QStringLiteral("gio"),
                                    {QStringLiteral("launch"), entry->path})) {
            return true;
        }
    }
    if (!entry->exec.isEmpty()
        && QProcess::startDetached(QStringLiteral("/bin/sh"),
                                   {QStringLiteral("-c"), entry->exec})) {
        return true;
    }
    if (error)
        *error = QStringLiteral("启动命令执行失败：%1").arg(entry->exec);
    return false;
#else
    // Windows: ShellExecute 直接执行 .lnk（会解析出真实目标与工作目录）
    // macOS: LaunchServices 打开 .app 包
    // 两者都交给系统，自己拼目标路径反而会丢掉参数/工作目录/以管理员身份等设置。
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(entry->path)))
        return true;
    if (error)
        *error = QStringLiteral("系统拒绝打开：%1").arg(QDir::toNativeSeparators(entry->path));
    return false;
#endif
}

void LocalAppIndex::reveal(const QString &id) const
{
    const Entry *entry = entryById(id);
    if (!entry)
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(entry->path).absolutePath()));
}

void LocalAppIndex::addEntry(const QString &name, const QString &path,
                             const QString &exec, const QString &iconName)
{
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty() || path.isEmpty())
        return;
    if (looksLikeJunk(trimmedName))
        return;
    if (m_entries.size() >= kMaxEntries)
        return;

    const QString id = idForPath(path);
    // 同名去重：同一个应用常同时出现在用户菜单与全局菜单里，
    // 按名称折叠比按目标路径更省事（解析 .lnk 的目标需要 COM，收益不值）。
    for (const Entry &existing : m_entries) {
        if (existing.id == id)
            return;
        if (existing.name.compare(trimmedName, Qt::CaseInsensitive) == 0)
            return;
    }

    Entry entry;
    entry.id = id;
    entry.name = trimmedName;
    entry.path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    entry.exec = exec;
    entry.iconName = iconName;
    // 拼音形态在这里算一次：搜索时每个按键都要扫全表，逐次转换会明显拖慢输入
    entry.initials = pinyin::initials(trimmedName);
    entry.pinyin = pinyin::full(trimmedName);
    // 次读音只在名称里真的有「换一种念法会变」的字时才存：
    // 与主读音完全相同时丢掉，避免给每个应用都白留两套字符串
    const QString altInitials = pinyin::initials(trimmedName, true);
    const QString altPinyin = pinyin::full(trimmedName, true);
    if (altInitials != entry.initials || altPinyin != entry.pinyin) {
        entry.altInitials = altInitials;
        entry.altPinyin = altPinyin;
    }
    m_entries.append(entry);
}

#if defined(Q_OS_WIN)

void LocalAppIndex::scanWindows()
{
    QStringList roots;

    // 当前用户的开始菜单
    const QString userMenu = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (!userMenu.isEmpty())
        roots.append(userMenu);

    // 所有用户的开始菜单。某些 Qt 版本的 standardLocations 不含它，显式补上。
    const QString programData = qEnvironmentVariable("ProgramData");
    if (!programData.isEmpty()) {
        roots.append(programData
                     + QStringLiteral("/Microsoft/Windows/Start Menu/Programs"));
    }
    roots.removeDuplicates();

    for (const QString &root : roots) {
        if (!QDir(root).exists())
            continue;

        QDirIterator it(root, QStringList{QStringLiteral("*.lnk")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && m_entries.size() < kMaxEntries) {
            const QFileInfo info(it.next());
            QString name = info.completeBaseName();
            // 复制/发送到桌面后系统会加后缀，展示时去掉
            name.remove(QRegularExpression(QStringLiteral(
                "\\s*[-—]\\s*(快捷方式|Shortcut)$"),
                QRegularExpression::CaseInsensitiveOption));
            addEntry(name, info.absoluteFilePath());
        }
    }
}

#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)

void LocalAppIndex::scanMac()
{
    QStringList roots = {
        QStringLiteral("/Applications"),
        QStringLiteral("/System/Applications"),
        QStringLiteral("/System/Applications/Utilities"),
        QDir::homePath() + QStringLiteral("/Applications"),
    };
    roots.removeDuplicates();

    for (const QString &root : roots) {
        QDir dir(root);
        if (!dir.exists())
            continue;

        const QFileInfoList children = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &child : children) {
            if (child.fileName().endsWith(QLatin1String(".app"))) {
                addEntry(child.completeBaseName(), child.absoluteFilePath());
                continue;
            }
            // 厂商目录（/Applications/Adobe.../X.app）再往下一层找
            QDir sub(child.absoluteFilePath());
            const QFileInfoList inner = sub.entryInfoList(
                QStringList{QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo &app : inner)
                addEntry(app.completeBaseName(), app.absoluteFilePath());
        }
    }
}

#elif defined(Q_OS_LINUX)

void LocalAppIndex::scanLinux()
{
    QStringList roots = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    if (roots.isEmpty())
        roots.append(QStringLiteral("/usr/share/applications"));
    roots.removeDuplicates();

    for (const QString &root : roots) {
        QDir dir(root);
        if (!dir.exists())
            continue;

        const QFileInfoList files = dir.entryInfoList(
            QStringList{QStringLiteral("*.desktop")}, QDir::Files);
        for (const QFileInfo &file : files) {
            if (m_entries.size() >= kMaxEntries)
                return;
            const DesktopDraft draft = parseDesktopFile(file.absoluteFilePath());
            if (!draft.usable)
                continue;
            addEntry(draft.name, file.absoluteFilePath(), draft.exec, draft.icon);
        }
    }
}

#endif

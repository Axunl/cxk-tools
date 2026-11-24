#pragma once

// ---------------------------------------------------------------------------
// 磁盘占用扫描：纯逻辑（不碰 QWidget，也不碰任何平台原生 API）。
//
// 为什么单独抽成一个 header：递归统计目录体积是整个插件里唯一"算错了也看不出来"
// 的部分（符号链接绕圈、权限拒绝、稀疏文件），必须能脱离宿主单独跑断言，
// 做法与 docs/PLUGIN.md 第 6 节一致。
//
// 统计口径（刻意写死，界面文案必须与之一致，否则用户会怀疑数字）：
//   * 只统计**普通文件的逻辑大小**（QFileInfo::size），不含目录项自身占的 4 KB、
//     不含磁盘块对齐后的真实占用 —— 与 Windows 资源管理器「大小」列同一口径；
//   * 符号链接一律不跟进、不计数：跟进会绕圈（自引用软链），记 size 会把目标
//     文件的字节算两遍，而"链接本身占多少"对清理毫无意义；
//   * 读不进去的子目录不阻断扫描，只把该项标成 unreadable，大小是"能读到的部分"。
//     这不是 bug：Windows 的 C:\System Volume Information、macOS 的部分
//     ~/Library 子目录本来就拒绝普通用户读取，把整次扫描判失败毫无意义。
//
// 另外提供一套"这一项能不能清"的分级判断（classify）：扫描本身只是量体积，
// 危险与否是另一件事，混在一起迟早会把保护逻辑写进界面里。
// ---------------------------------------------------------------------------

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <tuple>

namespace diskscan {

/// 清理风险分级。界面上的徽章、筛选、按钮可用性全部按它来。
enum class Level
{
    Normal,     ///< 普通目录，清不清由用户判断
    Recommend,  ///< 缓存 / 临时 / 日志 / 依赖目录：删了基本无副作用
    Caution,    ///< 构建产物、备份、个人资料：可能是正在用的东西，先看清再动
    Protected   ///< 系统目录、盘根、用户主目录本身：不提供清理入口
};

/// 一项"这是什么 / 该怎么处理"的说明。hintFor() 按目录名 / 路径给出，
/// tone 复用 Level：可清理（绿）、谨慎（黄）、别动（灰）。
struct Hint
{
    QString text;
    Level   tone = Level::Normal;
};

/// 扫描结果里的一项：被扫描目录下的一个直接子项（文件夹或散落文件）。
struct Item
{
    QString name;                  ///< 显示名（末级名字，不带路径）
    QString path;                  ///< 绝对路径，打开 / 清理都用它
    quint64 bytes = 0;              ///< 该项包含的字节数（文件夹为递归合计）
    quint64 files = 0;              ///< 该项包含的普通文件数（不含目录）
    bool    isDir = false;
    bool    unreadable = false;     ///< 至少有一个子目录读不进去，大小是偏小的
    int     unreadableDirs = 0;     ///< 读不进去的子目录数，用于悬浮提示
    int     skippedLinks = 0;       ///< 跳过的符号链接数，用于悬浮提示
    Level   level = Level::Normal;  ///< 清理风险分级（见 classify）
    QString hint;                  ///< 一句话说明（见 hintFor），可能为空
    Level   hintTone = Level::Normal;   ///< 说明的语气，决定界面上这句字的颜色
};

/// 扫描进度。total 是"顶层子项"的个数，不是文件总数：
/// 一个 100 万文件的目录也只有 1 个子项，进度条按文件数会长时间停在 0。
struct Progress
{
    QString current;   ///< 正在统计的路径（界面只显示末级名字）
    int     done = 0;
    int     total = 0;
    quint64 bytes = 0; ///< 目前已累计的字节数，用于"边扫边看"
};

/// 一次扫描的完整结果。
struct Result
{
    QString       root;            ///< 规范化后的绝对路径
    QString       error;           ///< 非空表示这次扫描根本没开始（路径不存在等）
    QVector<Item> items;           ///< 直接子项，未排序（排序由界面按列决定）
    quint64       totalBytes = 0;
    quint64       totalFiles = 0;
    int           unreadableDirs = 0;
    int           skippedLinks = 0;
    bool          canceled = false;
    qint64        elapsedMs = 0;
};

/// 字节数 → 人类可读文案（1.05 GB / 23.4 MB）。
/// 小于 10 的保留两位：12.34 GB 与 1.05 GB 放在一列里，一位小数会把后者压成 1.0。
inline QString formatBytes(quint64 bytes)
{
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};

    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);

    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = value < 10.0 ? 2 : 1;
    return QStringLiteral("%1 %2")
        .arg(QString::number(value, 'f', decimals), QString::fromLatin1(units[unit]));
}

/// 数量加千分位（1234567 → 1,234,567）：文件数动辄六位数，不加分隔符数不清位数。
inline QString formatCount(quint64 count)
{
    return QLocale().toString(static_cast<qulonglong>(count));
}

/// 路径比较用的规范形式：绝对路径 + 去尾分隔符 + （Windows/macOS）忽略大小写。
/// 只用于相等/包含判断，不要拿结果去访问文件系统。
inline QString canonicalForCompare(const QString &path)
{
    QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    // 盘根 "C:/" 与 POSIX 的 "/" 不能被削成空串
    while (normalized.size() > 1
           && (normalized.endsWith(QLatin1Char('/')) || normalized.endsWith(QLatin1Char('\\')))) {
        normalized.chop(1);
    }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Windows 与 macOS 的默认文件系统都不区分大小写（macOS 可格成区分，
    // 但误判成同一个目录最多是"更保守地拦一下"，不会造成误删）
    return normalized.toLower();
#else
    return normalized;
#endif
}

/// child 是否等于 parent 或位于 parent 之内（两者都应是绝对路径）。
inline bool isSameOrInside(const QString &child, const QString &parent)
{
    const QString c = canonicalForCompare(child);
    const QString p = canonicalForCompare(parent);
    if (c == p)
        return true;
    if (p == QLatin1String("/"))
        return c.startsWith(QLatin1Char('/'));
    return c.startsWith(p + QLatin1Char('/'));
}

namespace detail {

/// 绝对不允许一键清理的路径：系统目录、盘根、用户主目录及其下的个人资料目录。
/// 只拦"这一项本身"，不拦它的子目录 —— 清 C:\Windows\Temp、~/Downloads/xxx
/// 都是正常操作，而把整个 C:\Windows 或 ~/Downloads 丢进回收站会毁掉数据。
inline const QStringList &blockedPaths()
{
    static const QStringList paths = [] {
        QStringList list;
        const auto add = [&list](const QString &p) {
            if (!p.isEmpty())
                list.append(canonicalForCompare(p));
        };

        const QString home = QDir::homePath();
        add(home);
        // 用户主目录下的"个人资料"一级目录：整块清掉属于灾难，里面的子目录可以
        add(QFileInfo(home).absolutePath());   // C:/Users 或 /home
        // 用 QStandardPaths 而不是拼 "Desktop" 之类的字面量：Linux 的目录名由
        // XDG 配置决定（可能是 ~/桌面），macOS 是本地化过的名字
        for (const auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation,
                                   QStandardPaths::DownloadLocation, QStandardPaths::PicturesLocation,
                                   QStandardPaths::MusicLocation, QStandardPaths::MoviesLocation}) {
            add(QStandardPaths::writableLocation(location));
        }

#if defined(Q_OS_WIN)
        // AppData 的两个根：整块清掉等于抹掉所有程序的配置
        add(qEnvironmentVariable("APPDATA"));
        add(qEnvironmentVariable("LOCALAPPDATA"));
        add(qEnvironmentVariable("SystemRoot"));
        add(qEnvironmentVariable("ProgramFiles"));
        add(qEnvironmentVariable("ProgramFiles(x86)"));
        add(qEnvironmentVariable("ProgramData"));
        const QString windir = qEnvironmentVariable("windir");
        if (!windir.isEmpty())
            add(windir + QStringLiteral("/System32"));
        // 根目录下的系统文件与系统目录：删掉任何一个都可能导致进不去系统
        const QString driveRoot = QDir::toNativeSeparators(QDir::rootPath());
        for (const char *name : {"$Recycle.Bin", "System Volume Information", "Recovery",
                                 "PerfLogs", "Windows.old", "pagefile.sys", "hiberfil.sys",
                                 "swapfile.sys", "bootmgr", "bootnxt", "ntldr", "ntdetect.com",
                                 "dumpstack.log.tmp", "config.msi"}) {
            add(driveRoot + QString::fromLatin1(name));
        }
#elif defined(Q_OS_MACOS)
        for (const char *p : {"/System", "/Library", "/Applications", "/Users", "/private",
                              "/bin", "/sbin", "/usr", "/etc", "/var", "/tmp"})
            add(QString::fromLatin1(p));
#else
        for (const char *p : {"/bin", "/boot", "/dev", "/etc", "/home", "/lib", "/lib64",
                              "/opt", "/proc", "/root", "/sbin", "/sys", "/usr", "/var",
                              "/snap", "/lost+found"})
            add(QString::fromLatin1(p));
#endif
        return list;
    }();
    return paths;
}

/// 明显可以清理的目录名（缓存 / 临时 / 日志 / 依赖）。大小写不敏感。
inline const QRegularExpression &recommendPattern()
{
    static const QRegularExpression re(
        QStringLiteral("(^|/)(temp|tmp|cache|caches|logs?|crashdumps|crashpad|"
                       "node_modules|__pycache__|\\.pytest_cache|\\.mypy_cache|"
                       "npm-cache|pnpm-cache|pnpm-store|yarn-cache|npm-packages|"
                       "\\.gradle|\\.nuget|\\.cargo|\\.ivy2|"
                       "recycle|thumbs\\.db|desktop\\.ini)($|/)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

/// 看起来能清、但可能是正在用的东西（构建产物 / 备份 / 旧版本目录）。
inline const QRegularExpression &cautionPattern()
{
    static const QRegularExpression re(
        QStringLiteral("(^|/)(build|dist|target|out|bin|obj|coverage|output|bak|backup|backups|"
                       "old|dump|dumps|binlog|upload|uploads|\\d{4}\\.\\d{3,4}\\.\\d+)($|/)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

/// 个人资料目录：里面的东西可能是唯一的副本，一律按"谨慎"处理。
inline const QStringList &personalDirs()
{
    static const QStringList dirs = [] {
        QStringList list;
        for (const auto location : {QStandardPaths::DesktopLocation, QStandardPaths::DocumentsLocation,
                                   QStandardPaths::DownloadLocation, QStandardPaths::PicturesLocation,
                                   QStandardPaths::MusicLocation, QStandardPaths::MoviesLocation}) {
            const QString dir = QStandardPaths::writableLocation(location);
            if (!dir.isEmpty())
                list.append(canonicalForCompare(dir));
        }
        return list;
    }();
    return dirs;
}

/// 每遍历这么多个文件回一次头，检查取消标志并抛出一次进度。
/// 取 256 是折中：更小会让回调（跨线程排队）成为扫描本身的瓶颈，
/// 更大则用户按下停止后要等好几秒才生效。
inline constexpr int kTickEveryEntries = 256;

/// 递归过程中的累计器。整个子树共用一份，避免每层返回一个大结构体。
struct Tally
{
    quint64 bytes = 0;
    quint64 files = 0;
    int unreadableDirs = 0;
    int skippedLinks = 0;
    int sinceTick = 0;
};

/// 递归统计 dirPath 下所有普通文件的大小。
///
/// @param cancel   可为空；为真时立刻返回（已累计的数值保留，调用方自己判断 canceled）
/// @param onVisit  可为空；每 kTickEveryEntries 个文件回调一次，用于刷新"正在统计…"
///                 文案与检查取消。回调不得阻塞（它跑在工作线程里）。
inline void walk(const QString &dirPath, Tally &tally, std::atomic<bool> *cancel,
                 const std::function<void(const QString &)> &onVisit)
{
    if (cancel && cancel->load(std::memory_order_relaxed))
        return;

    const QDir dir(dirPath);
    // DirsFirst 只是让遍历顺序更接近用户直觉（先下钻再记散文件），不影响结果。
    // 不给 FollowSymlinks：给了就会跟进软链，自引用目录会无限递归。
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::DirsFirst);

    if (entries.isEmpty()) {
        // 空目录与"看得见却读不进去"是两件事：后者要记账，
        // 否则用户会以为这个目录真的只有 0 字节。
        if (!dir.isReadable())
            ++tally.unreadableDirs;
        return;
    }

    for (const QFileInfo &entry : entries) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return;

        if (entry.isSymLink()) {
            ++tally.skippedLinks;
            continue;
        }
        if (entry.isDir()) {
            walk(entry.absoluteFilePath(), tally, cancel, onVisit);
            continue;
        }

        // size() 对稀疏文件返回逻辑大小（可能远大于实际占用），与资源管理器一致；
        // 某些虚拟文件系统会返回 -1，按 0 计而不是让 quint64 溢出成大数。
        const qint64 size = entry.size();
        tally.bytes += size > 0 ? static_cast<quint64>(size) : 0;
        ++tally.files;

        if (onVisit && ++tally.sinceTick >= kTickEveryEntries) {
            tally.sinceTick = 0;
            onVisit(entry.absolutePath());
        }
    }
}

} // namespace detail

/// 一条说明规则：正则 + 文案 + 语气。规则表刻意按"从具体到通用"排，
/// 先命中先返回（例如 hiberfil.sys 要排在 sys 这类通用规则前面）。
namespace detail {

struct HintRule
{
    QRegularExpression re;
    QString text;
    Level tone;
};

inline QVector<HintRule> makeRules(std::initializer_list<std::tuple<const char *, const char *, Level>> items)
{
    QVector<HintRule> rules;
    rules.reserve(static_cast<int>(items.size()));
    for (const auto &entry : items) {
        rules.append({QRegularExpression(QString::fromUtf8(std::get<0>(entry)),
                                         QRegularExpression::CaseInsensitiveOption),
                      QString::fromUtf8(std::get<1>(entry)), std::get<2>(entry)});
    }
    return rules;
}

/// 只在**完整路径**上匹配的规则：这些名字太常见（installer、download），
/// 只看末级名字会把用户自己的同名目录误判成系统目录。
inline const QVector<HintRule> &hintPathRules()
{
    static const QVector<HintRule> rules = makeRules({
        {"(^|/)windows/installer$",
         "Windows 安装缓存，卸载和修复程序要用它；整块删掉会让程序卸不掉也修不了", Level::Protected},
        {"(^|/)windows/softwaredistribution/download$",
         "Windows 更新下载缓存，用「磁盘清理 → Windows 更新清理」删更稳", Level::Recommend},
        {"(^|/)windows/(panther|minidump|logs/(cbs|dism))$",
         "Windows 安装与更新日志，排查完可以删", Level::Recommend},
    });
    return rules;
}

/// 只对**末级名字**匹配的规则。
inline const QVector<HintRule> &hintNameRules()
{
    static const QVector<HintRule> rules = makeRules({
        // ---- 系统文件与系统目录：别动
        {"^pagefile\\.sys$",
         "系统虚拟内存文件，由系统管理；改大小在「系统属性 → 高级 → 虚拟内存」，别直接删", Level::Protected},
        {"^hiberfil\\.sys$",
         "休眠文件，大小约等于内存；用 powercfg /h off 关掉休眠即可释放，别手删", Level::Protected},
        {"^swapfile\\.sys$",
         "虚拟内存的交换文件，跟着 pagefile 走，不要删", Level::Protected},
        {"^(memory\\.dmp|dumpstack\\.log\\.tmp)$",
         "系统崩溃转储；不想再生成可在「系统属性 → 启动和故障恢复」里关掉", Level::Protected},
        {"^system volume information$",
         "系统还原点与卷影副本，用「磁盘清理 → 更多选项」调整，别直接删", Level::Protected},
        {"^winsxs$",
         "组件存储，看着大但多数和系统文件是硬链接；用 DISM /Online /Cleanup-Image /StartComponentCleanup，手删会损坏系统", Level::Protected},
        {"^windows\\.old$",
         "升级前的旧系统；要在「磁盘清理 → 以前的 Windows 安装」里删才安全", Level::Protected},
        {"^recovery$", "系统恢复环境，不要删", Level::Protected},
        {"^(windows|system32|syswow64)$", "系统目录，不要动", Level::Protected},
        {"^(program files( \\(x86\\))?|programdata|appdata)$",
         "程序配置 / 公共数据；按子目录清，整块删会搞坏所有程序", Level::Protected},
        {"^\\$recycle\\.bin$",
         "回收站内容；右键回收站选「清空」后空间才真正释放", Level::Caution},

        // ---- 明确可以清的
        {"^(temp|tmp)$",
         "临时文件，可以清；正在运行的程序可能正占着其中几个", Level::Recommend},
        {"^(cache|caches|cachestorage|code cache|gpu cache|shadercache|gpucache)$",
         "缓存目录，删了程序会自动重建，只影响下次启动的速度", Level::Recommend},
        {"^(log|logs)$",
         "日志文件，清掉不影响运行；没有轮转策略的话会一直涨", Level::Recommend},
        {"^node_modules$",
         "前端依赖目录，删掉后 npm / pnpm install 能重新装回来", Level::Recommend},
        {"^__pycache__$|^\\.(pytest_cache|mypy_cache)$", "Python 字节码与检查缓存，删了自动重建", Level::Recommend},
        {"^(npm-cache|_cacache|pnpm-store|pnpm-cache|yarn-cache|\\.npm|\\.yarn)$",
         "包管理器下载缓存，清掉后下次安装重新下载", Level::Recommend},
        {"^(\\.gradle|\\.m2|\\.ivy2|\\.nuget|\\.cargo|gradle caches)$",
         "构建依赖缓存，清掉后下次构建会重新下载", Level::Recommend},
        {"^(crashdumps|crashpad|minidumps?|crashes|watson)$",
         "崩溃转储，排查完就能删", Level::Recommend},
        {"(\\.dmp|\\.hprof|\\.mdmp)$|^java_error_in_.*|^hs_err_pid.*",
         "崩溃转储 / Java 堆快照；排查完就不用留了", Level::Recommend},
        {"^(jetbrains|intellijidea.*|pycharm.*|goland.*|clion.*|webstorm.*|rider.*|androidstudio)$",
         "IDE 的索引与缓存，删了会重建（下次打开项目稍慢一点）", Level::Recommend},

        // ---- 可能正在用，先看清再动
        {"^binlog$|^binlog\\.\\d+$|^mysql-bin\\.\\d+$|^mysql-bin\\.index$",
         "MySQL 二进制日志（主从复制、按时间点恢复要靠它）；用 PURGE BINARY LOGS 或调 expire_logs_days 清，直接删可能丢数据", Level::Caution},
        {"^ibdata\\d*$|^ib_logfile\\d+$|^#innodb_temp$",
         "InnoDB 的表空间 / 重做日志，删了数据库直接起不来", Level::Protected},
        {"^(mysql|mariadb|postgresql|pgdata)$",
         "数据库数据目录，里面的大头通常是日志与数据文件；交给数据库自己清，别直接删", Level::Caution},
        {"^(\\.venv|venv|virtualenv)$", "Python 虚拟环境，删了可以用 requirements 重建", Level::Caution},
        {"^(\\.git|\\.svn|\\.hg)$", "版本库历史，删掉本地提交记录就没了", Level::Caution},
        {"^(backup|backups|bak|old)$|\\.bak$", "备份或旧版本，确认新版本正常后再删", Level::Caution},
        {"^(build|dist|target|out|obj|release|debug)$", "构建产物，可以重新编译生成", Level::Caution},
        {"^(virtual machines?|vmware|virtualbox|hyper-v)$", "虚拟机磁盘目录，确认里面没有还要用的机器", Level::Caution},
        {"(\\.vmdk|\\.vdi|\\.vhdx|\\.vhd|\\.qcow2|\\.iso|\\.ova)$",
         "虚拟机磁盘 / 系统镜像，确认不再需要再删", Level::Caution},
        {"^ext4\\.vhdx$|^(docker|wsl|dockerdesktop)$",
         "WSL / Docker 的虚拟磁盘；用 docker system prune 或压缩虚拟盘回收更稳", Level::Caution},
        {"^(wechat files|xwechat_files|weixin)$",
         "微信的聊天文件与图片；用微信「设置 → 存储空间」清更安全", Level::Caution},
        {"^(tencent files|qq|tim|dingtalk|feishu|lark)$",
         "聊天软件的缓存与文件，用软件自带的清理更安全", Level::Caution},
        {"^(onedrive|dropbox|baidunetdisk)$", "云同步目录，本地删掉云端也会一起删", Level::Caution},
        {"^(desktop|documents|downloads|pictures|music|videos|movies|桌面|文档|下载|图片|音乐|视频)$",
         "个人资料目录，删前确认别处有备份", Level::Caution},
    });
    return rules;
}

} // namespace detail

/// 给出一项"这是什么 / 该怎么处理"的说明；没有把握时返回空（界面就不显示）。
/// 纯字符串判断，不碰磁盘 —— 这些结论是经验规则，界面上要标明"仅供参考"。
inline Hint hintFor(const QString &path)
{
    const QString normalized = canonicalForCompare(path);
    if (normalized.isEmpty())
        return {};

    for (const detail::HintRule &rule : detail::hintPathRules()) {
        if (rule.re.match(normalized).hasMatch())
            return {rule.text, rule.tone};
    }

    const QString name = normalized.section(QLatin1Char('/'), -1);
    for (const detail::HintRule &rule : detail::hintNameRules()) {
        if (rule.re.match(name).hasMatch())
            return {rule.text, rule.tone};
    }
    return {};
}

/// 给出这一项的清理风险等级。纯字符串判断，不碰磁盘。
inline Level classify(const QString &path)
{
    // 盘根（C:/、D:/、/）永远受保护：清掉它等于清掉整块盘。
    // 这一步不能只靠 blockedPaths 比对 —— "C:/" 规范化后是 "c:"，很容易漏
    if (QDir(path).isRoot())
        return Level::Protected;

    const QString normalized = canonicalForCompare(path);

    for (const QString &blocked : detail::blockedPaths()) {
        if (normalized == blocked)
            return Level::Protected;
    }

    // 名字规则比路径表更细：WinSxS / hiberfil.sys 都不在 blockedPaths 里，
    // 但删了照样出事；反过来 node_modules 落在桌面下也是能放心删的。
    // 所以先听名字规则的，再走通用正则与个人目录兜底。
    const Hint hint = hintFor(path);
    if (hint.tone == Level::Protected)
        return Level::Protected;
    if (hint.tone == Level::Recommend)
        return Level::Recommend;

    if (detail::recommendPattern().match(normalized).hasMatch())
        return Level::Recommend;

    for (const QString &personal : detail::personalDirs()) {
        if (normalized.startsWith(personal + QLatin1Char('/')))
            return Level::Caution;
    }

    if (hint.tone == Level::Caution)
        return Level::Caution;

    if (detail::cautionPattern().match(normalized).hasMatch())
        return Level::Caution;

    return Level::Normal;
}

/// 等级 → 徽章文案。空串表示这一行不打标记（普通项）。
inline QString levelLabel(Level level)
{
    switch (level) {
    case Level::Recommend: return QStringLiteral("可清理");
    case Level::Caution:   return QStringLiteral("谨慎");
    case Level::Protected: return QStringLiteral("受保护");
    case Level::Normal:    break;
    }
    return QString();
}

/// 判断"这一项能不能移到回收站"，返回空串表示可以，否则是给用户看的原因。
/// 与 classify() 同源：只有 Protected 会被拦下，Caution 只是劝一句。
inline QString trashBlockReason(const QString &path)
{
    if (QDir(path).isRoot())
        return QStringLiteral("磁盘根目录不能整体清理");
    if (canonicalForCompare(path).isEmpty())
        return QStringLiteral("路径无效");
    if (classify(path) == Level::Protected)
        return QStringLiteral("这是系统目录或用户资料根目录，不能在这里清理");
    return QString();
}

/// 统计单个顶层子项（文件夹递归 / 文件直接取 size）。
/// @param onVisit 可为空，语义同 detail::walk()。
inline Item measure(const QFileInfo &info, std::atomic<bool> *cancel,
                    const std::function<void(const QString &)> &onVisit = {})
{
    Item item;
    item.name = info.fileName();
    item.path = info.absoluteFilePath();
    item.isDir = info.isDir();

    detail::Tally tally;
    if (item.isDir) {
        detail::walk(item.path, tally, cancel, onVisit);
    } else if (!info.isSymLink()) {
        const qint64 size = info.size();
        tally.bytes = size > 0 ? static_cast<quint64>(size) : 0;
        tally.files = 1;
    } else {
        // 顶层就是一个软链：不计体积（同 walk 的口径），但仍要让用户看见它
        tally.skippedLinks = 1;
    }

    item.bytes = tally.bytes;
    item.files = tally.files;
    item.unreadable = tally.unreadableDirs > 0;
    item.unreadableDirs = tally.unreadableDirs;
    item.skippedLinks = tally.skippedLinks;
    item.level = classify(item.path);
    // 说明在后台线程算好带回去：表格每次刷新（扫描中每 120ms 一次）都要重画，
    // 到那时再跑几十条正则纯属浪费
    const Hint hint = hintFor(item.path);
    item.hint = hint.text;
    item.hintTone = hint.tone;
    return item;
}

/// 扫描 rootPath 的**直接子项**并各自统计体积 —— 也就是"谁最占地方"这一层。
///
/// 不递归把整棵树都返回：一棵几百万节点的树既传不回 UI 线程，也没人看得完。
/// 想看某一项里面，就把它当新的 root 再扫一次（界面上的"双击进入"）。
///
/// @param cancel     可为空
/// @param onProgress 可为空；两条线程间唯一的进度通道，调用方负责跨线程投递
/// @param onItem     可为空；每算完一个子项回调一次，用于让它立刻出现在列表里
inline Result scanChildren(const QString &rootPath, std::atomic<bool> *cancel,
                           const std::function<void(const Progress &)> &onProgress = {},
                           const std::function<void(const Item &)> &onItem = {})
{
    const auto started = std::chrono::steady_clock::now();

    Result result;
    const QFileInfo rootInfo(rootPath);
    result.root = QDir::cleanPath(rootInfo.absoluteFilePath());

    if (!rootInfo.exists()) {
        result.error = QStringLiteral("路径不存在：%1").arg(result.root);
        return result;
    }
    if (!rootInfo.isDir()) {
        result.error = QStringLiteral("只能扫描文件夹：%1").arg(result.root);
        return result;
    }

    const QDir dir(result.root);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::DirsFirst);
    if (entries.isEmpty() && !dir.isReadable()) {
        result.error = QStringLiteral("没有权限读取：%1").arg(result.root);
        return result;
    }

    Progress progress;
    progress.total = entries.size();

    // 进度节流：一个目录里几万个文件时，每个 tick 都跨线程投递一次会把
    // UI 线程的事件队列塞满，界面反而更卡。100ms 一次肉眼已经够连贯。
    auto lastEmit = started;
    const auto emitProgress = [&](bool force) {
        if (!onProgress)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count() < 100)
            return;
        lastEmit = now;
        onProgress(progress);
    };

    result.items.reserve(entries.size());
    for (const QFileInfo &info : entries) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.canceled = true;
            break;
        }

        const Item item = measure(info, cancel, [&](const QString &current) {
            progress.current = current;
            emitProgress(false);
        });

        result.items.append(item);
        result.totalBytes += item.bytes;
        result.totalFiles += item.files;
        result.unreadableDirs += item.unreadableDirs;
        result.skippedLinks += item.skippedLinks;

        progress.done += 1;
        progress.bytes = result.totalBytes;
        progress.current = item.path;
        if (onItem)
            onItem(item);
        emitProgress(false);
    }

    // 收尾强制发一次：否则最后一段进度（尤其是 total==done 的那一帧）会被节流吃掉
    emitProgress(true);

    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    return result;
}

/// 「占用大头」链路：从一个目录出发，一路只跟进最大的子项，
/// 回答的是"这一百多 GB 到底被哪一层吃掉了"。
///
/// 与 scanChildren 的关系：那个是"这一层的排行"，这个是"顺着最大的一条往下钻"。
/// 两者口径一致（同一套 walk 规则、同样不跟进符号链接）。
struct Hotspot
{
    QString          root;        ///< 起点目录
    QString          error;       ///< 非空表示没找到（路径不可读等）
    QStringList      names;       ///< 链路每一级的名字（不含 root）
    QStringList      paths;       ///< 与 names 对应的绝对路径
    QVector<quint64> sizes;       ///< 与 names 对应的体积
    QVector<quint64> fileCounts;  ///< 与 names 对应的文件数
    quint64          rootBytes = 0;
    int              unreadableDirs = 0;
    bool             isDir = false;   ///< 末端是不是文件夹（决定界面能不能"进去"）
    Hint             hint;            ///< 末端那项的说明
    bool             canceled = false;
    qint64           elapsedMs = 0;

    bool isEmpty() const { return paths.isEmpty(); }
    QString leafPath() const { return paths.isEmpty() ? root : paths.last(); }
};

namespace detail {

/// 顺着最大子项往下最多看几层。再深就是"某个日志文件本身"了，界面也用不上。
inline constexpr int kHotMaxDepth = 4;
/// 链路上最多展示几级
inline constexpr int kHotMaxLevels = 4;
/// 低于这个量的一律不算"大头"：清理 300 MB 以下的目录收益基本为 0
inline constexpr quint64 kHotMinBytes = 300ull * 1024 * 1024;
/// 往下钻的占比门槛：子项不到上一层 25% 就说明空间是分散的，继续钻没有意义
inline constexpr double kHotMinShare = 0.25;

/// 递归过程中的"最大链"。只留一条往下最大的链，其余算完就丢，
/// 所以一棵上百万节点的树也只有 O(层数) 的额外内存。
struct HotNode
{
    quint64 bytes = 0;
    quint64 files = 0;
    int unreadable = 0;
    QStringList names;
    QStringList paths;
    QVector<quint64> sizes;
    QVector<quint64> fileCounts;
};

struct HotState
{
    std::atomic<bool> *cancel = nullptr;
    const std::function<void(const QString &, quint64)> *onVisit = nullptr;
    quint64 scanned = 0;
    int sinceTick = 0;
};

inline void walkHot(const QString &dirPath, int depth, HotState &state, HotNode &node)
{
    if (state.cancel && state.cancel->load(std::memory_order_relaxed))
        return;

    const QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::DirsFirst);
    if (entries.isEmpty()) {
        if (!dir.isReadable())
            ++node.unreadable;
        return;
    }

    quint64 bestBytes = 0;
    quint64 bestFiles = 0;
    QString bestName;
    QString bestPath;
    HotNode bestNode;

    for (const QFileInfo &entry : entries) {
        if (state.cancel && state.cancel->load(std::memory_order_relaxed))
            return;

        if (entry.isSymLink())
            continue;   // 与 walk() 同口径：不跟进、不计数

        HotNode child;   // 目录的子结果，只有成为"最大"时才往上带
        quint64 bytes = 0;
        quint64 files = 0;
        const bool isDir = entry.isDir();
        if (isDir) {
            walkHot(entry.absoluteFilePath(), depth + 1, state, child);
            bytes = child.bytes;
            files = child.files;
            node.unreadable += child.unreadable;
        } else {
            const qint64 size = entry.size();
            bytes = size > 0 ? static_cast<quint64>(size) : 0;
            files = 1;
            state.scanned += bytes;
            if (state.onVisit && *state.onVisit) {
                if (++state.sinceTick >= kTickEveryEntries) {
                    state.sinceTick = 0;
                    (*state.onVisit)(entry.absolutePath(), state.scanned);
                }
            }
        }

        node.bytes += bytes;
        node.files += files;

        if (bytes > bestBytes) {
            bestBytes = bytes;
            bestFiles = files;
            bestName = entry.fileName();
            bestPath = entry.absoluteFilePath();
            bestNode = isDir ? child : HotNode();
        }
    }

    // 链只在靠上的几层建：再深的目录即使算出来也没人看，白搭内存
    if (depth < kHotMaxDepth && bestBytes > 0) {
        node.names = bestNode.names;
        node.paths = bestNode.paths;
        node.sizes = bestNode.sizes;
        node.fileCounts = bestNode.fileCounts;
        node.names.prepend(bestName);
        node.paths.prepend(bestPath);
        node.sizes.prepend(bestBytes);
        node.fileCounts.prepend(bestFiles);
    }
}

} // namespace detail

/// 找出 rootPath 里"占用大头"的那条链路。
///
/// 一次遍历就够（不是每层重扫一遍）：递归返回每个目录的体积，顺带记下它最大的子项，
/// 逐层拼起来就是一条从 root 往下最大的一条链。
///
/// @param cancel  可为空
/// @param onVisit 可为空；每 kTickEveryEntries 个文件回调一次（路径, 已扫字节），
///                跑在工作线程，不得阻塞
inline Hotspot findHotspot(const QString &rootPath, std::atomic<bool> *cancel,
                           const std::function<void(const QString &, quint64)> &onVisit = {})
{
    const auto started = std::chrono::steady_clock::now();

    Hotspot spot;
    const QFileInfo rootInfo(rootPath);
    spot.root = QDir::cleanPath(rootInfo.absoluteFilePath());
    if (!rootInfo.exists()) {
        spot.error = QStringLiteral("路径不存在：%1").arg(spot.root);
        return spot;
    }
    if (!rootInfo.isDir()) {
        spot.error = QStringLiteral("只能分析文件夹：%1").arg(spot.root);
        return spot;
    }

    detail::HotState state;
    state.cancel = cancel;
    state.onVisit = &onVisit;
    detail::HotNode node;
    detail::walkHot(spot.root, 0, state, node);

    spot.rootBytes = node.bytes;
    spot.unreadableDirs = node.unreadable;
    spot.canceled = cancel && cancel->load(std::memory_order_relaxed);

    // 只保留"每一级都占上一层 25% 以上、且自己不小于 300 MB"的前几级：
    // 3% → 2% → 1% 这种链条对用户毫无意义，只会把真正的大头稀释掉。
    // 第一级无条件保留 —— "最大的那个只占 12%" 本身也是一条有用结论。
    quint64 parent = node.bytes;
    for (int i = 0; i < node.sizes.size() && spot.sizes.size() < detail::kHotMaxLevels; ++i) {
        const quint64 size = node.sizes.at(i);
        if (!spot.sizes.isEmpty()) {
            if (parent == 0 || static_cast<double>(size) / static_cast<double>(parent) < detail::kHotMinShare)
                break;
            if (size < detail::kHotMinBytes)
                break;
        }
        spot.names.append(node.names.at(i));
        spot.paths.append(node.paths.at(i));
        spot.sizes.append(size);
        spot.fileCounts.append(node.fileCounts.at(i));
        parent = size;
    }

    if (!spot.paths.isEmpty()) {
        const QString leaf = spot.paths.last();
        spot.isDir = QFileInfo(leaf).isDir();
        spot.hint = hintFor(leaf);
    }

    spot.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count();
    return spot;
}

} // namespace diskscan

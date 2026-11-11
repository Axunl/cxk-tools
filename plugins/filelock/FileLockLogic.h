#pragma once

// ---------------------------------------------------------------------------
// 文件占用释放：纯逻辑层。
//
// 只做「路径文本 → 目标集合 → 匹配判定 → 结果归并」四件事，只依赖 Qt Core，
// 不 include 任何平台头、不碰 QWidget。理由同 ProcText.h / JsonText.h：
// 路径归一化（尤其 GetFinalPathNameByHandle 给的 \\?\ 前缀、Windows 大小写）
// 最容易写错，抽成 header 后可以在临时工程里直接 include 断言，不必真的去占文件。
//
// 平台差异一律由参数传进来（caseInsensitive），本文件里不写 #if：
// 同一份逻辑在 Windows 与 Unix 两种语义下都能被测到。
// ---------------------------------------------------------------------------

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace filelock {

/// 一次检测最多展开多少个文件。
/// Windows 的 Restart Manager 要把每个文件都注册进会话，几万个文件会让会话
/// 卡住数秒；512 已覆盖「一个项目目录 / 一个下载文件夹」的常见场景，
/// 超出时明确告诉用户缩小范围，而不是悄悄只查前几个（那会漏报占用）。
constexpr int kMaxExpandedFiles = 512;

/// 单个占用者（已按 pid 归并）。
struct Holder
{
    quint64 pid = 0;
    QString name;       ///< 进程名；拿不到时是 "(pid N)"
    QString type;       ///< 「窗口程序」「服务」「进程」等，随平台检测手段而定
    QString hitPath;    ///< 命中的文件；同 pid 命中多个时是「N 个文件」
    int hitCount = 1;   ///< 同 pid 命中的文件数
    QString status;     ///< 占用中 / 已解除 / 已结束
};

/// 展开后的检测目标。
struct Targets
{
    QStringList files;       ///< 待查询的文件绝对路径
    QStringList dirs;        ///< 原始目录绝对路径（用于「目录本身被打开」的匹配）
    bool truncated = false;  ///< 因 kMaxExpandedFiles 被截断
    QString error;           ///< 非空表示输入无效，直接显示给用户
};

/// 归一化后的匹配集合，元素都是 pathKey() 的结果。
struct MatchSet
{
    QStringList files;
    QStringList dirs;       ///< 不带末尾斜杠
};

/// 一次检测的结果。error 非空表示失败或「结果可能不完整」，面板要如实显示。
struct ScanOutcome
{
    QVector<Holder> holders;
    QString error;
    int scannedFiles = 0;
    bool truncated = false;
};

/// 一次「解除占用 / 强制结束」的结果。
struct ActionOutcome
{
    QVector<quint64> succeededPids;  ///< 真正动过的进程（面板据此改状态列）
    int failed = 0;
    int closedHandles = 0;           ///< 实际关掉的句柄数，仅解除占用有意义
    QString error;                   ///< 第一处失败原因或平台不支持原因

    int succeeded() const { return succeededPids.size(); }
};

/// 把输入框里的多路径文本拆成去重后的路径列表。
///
/// 分隔符只认分号与换行：逗号在 Windows 文件名里合法，拿它当分隔符会
/// 把 "报表,v2.xlsx" 切成两半。资源管理器「复制文件地址」会带双引号，也要剥掉。
inline QStringList parsePathText(const QString &text)
{
    QString normalized = text;
    normalized.replace(QLatin1Char('\n'), QLatin1Char(';'));
    const QStringList raw = normalized.split(QLatin1Char(';'), Qt::SkipEmptyParts);

    QStringList out;
    for (QString part : raw) {
        part = part.trimmed();
        if (part.size() >= 2 && part.startsWith(QLatin1Char('"'))
            && part.endsWith(QLatin1Char('"'))) {
            part = part.mid(1, part.size() - 2).trimmed();
        }
        if (part.isEmpty())
            continue;
        if (!out.contains(part))
            out.append(part);
    }
    return out;
}

/// 把路径变成可比较的键。
///
/// 三件事：统一分隔符、解析符号链接（让 /a/link 与 /a/real 视为同一文件）、
/// 去掉末尾斜杠。caseInsensitive 由调用方按平台传：Windows 的 NTFS 大小写
/// 不敏感，Unix 大小写敏感，提前 toCaseFolded 会把 Linux 上的两个文件混成一个。
inline QString pathKey(QString path, bool caseInsensitive)
{
    path = path.trimmed();
    // 手写成 '/' 而不是 QDir::fromNativeSeparators：后者在 Unix 上不会转换 '\'，
    // 而这个函数还要能在 Windows 上解析 GetFinalPathNameByHandle 的返回串，
    // 手动替换才能让三平台行为一致、也才能让临时测试断言跨平台结果。
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.isEmpty())
        return QString();

    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        path = canonical;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    path = QDir::cleanPath(path);
    return caseInsensitive ? path.toCaseFolded() : path;
}

/// 把 GetFinalPathNameByHandleW 的返回值转成普通路径。
///
/// 它给的是 "\\?\C:\dir\file"（UNC 是 "\\?\UNC\host\share"），带设备前缀，
/// 直接和 QFileInfo 的路径比会全部不匹配。Windows 之外不会调到，但保持
/// 平台无关实现以便临时工程直接断言。
inline QString fromWin32FinalPath(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.startsWith(QLatin1String("//?/UNC/"), Qt::CaseInsensitive))
        return QLatin1String("//") + path.mid(8);
    if (path.startsWith(QLatin1String("//?/"), Qt::CaseInsensitive))
        return path.mid(4);
    return path;
}

/// 把用户输入（文件 / 文件夹混合）展开成待检测的文件与目录集合。
///
/// recursive 只影响文件夹：不勾时只列顶层文件，勾上时递归子目录。
/// QDirIterator 默认不跟随目录符号链接，正好避免链接成环时无限遍历。
inline Targets expandTargets(const QStringList &inputs, bool recursive, bool caseInsensitive)
{
    Targets out;
    QSet<QString> seenFiles;
    QSet<QString> seenDirs;

    for (const QString &raw : inputs) {
        const QFileInfo info(raw);
        if (!info.exists()) {
            out.error = QStringLiteral("路径不存在：%1").arg(raw);
            return out;
        }

        // canonicalFilePath 对不存在/无权限的对象返回空，退化成 absoluteFilePath
        const QString absolute = info.canonicalFilePath().isEmpty()
                                     ? info.absoluteFilePath()
                                     : info.canonicalFilePath();

        if (info.isDir()) {
            const QString key = pathKey(absolute, caseInsensitive);
            if (!seenDirs.contains(key)) {
                seenDirs.insert(key);
                out.dirs.append(QDir::fromNativeSeparators(absolute));
            }

            QDirIterator it(absolute, QDir::Files | QDir::Hidden | QDir::System,
                            recursive ? QDirIterator::Subdirectories
                                      : QDirIterator::NoIteratorFlags);
            while (it.hasNext()) {
                const QString file = it.next();
                if (out.files.size() >= kMaxExpandedFiles) {
                    out.truncated = true;   // 后面还有，但不再查，避免界面假死
                    break;
                }
                const QString fileKey = pathKey(file, caseInsensitive);
                if (fileKey.isEmpty() || seenFiles.contains(fileKey))
                    continue;
                seenFiles.insert(fileKey);
                out.files.append(QDir::fromNativeSeparators(file));
            }
            if (out.truncated)
                break;
        } else {
            const QString file = QDir::fromNativeSeparators(absolute);
            const QString key = pathKey(file, caseInsensitive);
            if (!key.isEmpty() && !seenFiles.contains(key)) {
                seenFiles.insert(key);
                out.files.append(file);
            }
        }
    }

    if (out.files.isEmpty() && out.dirs.isEmpty() && out.error.isEmpty())
        out.error = QStringLiteral("请先输入或拖入文件 / 文件夹路径");
    return out;
}

/// 由展开结果构造比较用的归一化集合。
inline MatchSet buildMatchSet(const Targets &targets, bool caseInsensitive)
{
    MatchSet set;
    for (const QString &file : targets.files)
        set.files.append(pathKey(file, caseInsensitive));
    for (const QString &dir : targets.dirs)
        set.dirs.append(pathKey(dir, caseInsensitive));
    return set;
}

/// 判断一个「被打开的路径」是否命中目标集合。
///
/// openedKey 必须已用 pathKey() 归一化。目录匹配分两种：命中目录本身，
/// 或命中其下的文件 —— 非递归时只认直接子项（剩余部分不含 '/'）。
/// 之所以要区分，是因为 Linux 的 /proc/<pid>/fd 会给出目录下每一层的真实路径，
/// 不区分就会把「只查顶层」变成隐式递归。
inline bool matchesPath(const QString &openedKey, const MatchSet &set,
                        bool recursive, bool caseInsensitive)
{
    if (openedKey.isEmpty())
        return false;

    for (const QString &file : set.files) {
        if (openedKey == file)
            return true;
    }

    const Qt::CaseSensitivity cs = caseInsensitive ? Qt::CaseInsensitive
                                                   : Qt::CaseSensitive;
    for (const QString &dir : set.dirs) {
        if (dir.isEmpty())
            continue;
        if (openedKey == dir)
            return true;
        const QString prefix = dir + QLatin1Char('/');
        if (!openedKey.startsWith(prefix, cs))
            continue;
        if (recursive)
            return true;
        if (!openedKey.mid(prefix.size()).contains(QLatin1Char('/')))
            return true;
    }
    return false;
}

/// 按 pid 归并命中记录：同 pid 占着多个文件时只显示一行。
///
/// 三种平台的原始记录粒度不同（RM 按进程、procfs 按 fd、lsof 按文件），
/// 归并放在这里能保证表格无论哪个平台都长得一样。
inline QVector<Holder> mergeHolders(QVector<Holder> raw)
{
    QHash<quint64, int> index;   // pid → 在 merged 里的行号
    QVector<Holder> merged;
    for (const Holder &holder : std::as_const(raw)) {
        if (holder.pid == 0)
            continue;
        auto it = index.constFind(holder.pid);
        if (it == index.constEnd()) {
            index.insert(holder.pid, merged.size());
            merged.append(holder);
            continue;
        }
        Holder &slot = merged[it.value()];
        slot.hitCount += qMax(holder.hitCount, 1);
        if (slot.name.isEmpty() || slot.name.startsWith(QLatin1String("(pid ")))
            slot.name = holder.name;
        if (slot.type.isEmpty())
            slot.type = holder.type;
        if (slot.hitPath != holder.hitPath)
            slot.hitPath.clear();   // 命中多个文件，下面换成「N 个文件」
    }

    for (Holder &holder : merged) {
        if (holder.name.isEmpty())
            holder.name = QStringLiteral("(pid %1)").arg(holder.pid);
        if (holder.type.isEmpty())
            holder.type = QStringLiteral("进程");
        if (holder.status.isEmpty())
            holder.status = QStringLiteral("占用中");
        if (holder.hitCount > 1 && holder.hitPath.isEmpty())
            holder.hitPath = QStringLiteral("%1 个文件").arg(holder.hitCount);
    }

    // 固定按 pid 排序：不同平台的枚举顺序不一样，不排的话每次检测结果会跳
    std::sort(merged.begin(), merged.end(),
              [](const Holder &a, const Holder &b) { return a.pid < b.pid; });
    return merged;
}

} // namespace filelock

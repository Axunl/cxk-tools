#include "ShareStore.h"

#include "ShareHttp.h"   // humanSize 之外只用它的纯逻辑约定，这里主要是保持依赖方向清晰

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

namespace share {

namespace {

/// 上传目录的绝对路径（不创建）。抽成自由函数是为了让 isManagedUpload() 能判断
/// 而不触发 uploadDir() 的建目录副作用。
QString uploadDirPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        return QString();
    return QDir::cleanPath(base + QStringLiteral("/share/uploads"));
}

/// 路径归一化成可比较的键：统一正斜杠、解析掉 "." 与 ".."。
/// 不解析符号链接（QFileInfo::canonicalFilePath 会做真实 IO，几百个文件时明显慢，
/// 而且文件被删掉后返回空串会让去重失效）。
QString pathKey(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

// ------------------------------------------------------------------ 单例

ShareStore &ShareStore::instance()
{
    // 刻意 new 出来永不 delete：这个对象在进程退出时若被析构，会牵动 QList 里的
    // QString 在 Qt 全局析构之后才释放（插件模块的静态对象析构顺序不确定），
    // 反而可能在退出阶段崩溃。交给操作系统回收是这种进程级单例的常规做法。
    static ShareStore *store = new ShareStore;
    return *store;
}

ShareStore::ShareStore(QObject *parent)
    : QObject(parent)
{
}

QString ShareStore::newId() const
{
    // 12 位十六进制（48 bit 随机）：碰撞概率在几百条量级可以忽略，
    // 但仍逐个比对一次，避免"理论上不可能"变成调试时抓不到的串号。
    for (;;) {
        const QString candidate = QUuid::createUuid()
                                      .toString(QUuid::WithoutBraces)
                                      .remove(QLatin1Char('-'))
                                      .left(12);
        bool taken = false;
        for (const FileEntry &entry : m_files) {
            if (entry.id == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            for (const TextEntry &entry : m_texts) {
                if (entry.id == candidate) {
                    taken = true;
                    break;
                }
            }
        }
        if (!taken)
            return candidate;
    }
}

// ------------------------------------------------------------------ 文件

bool ShareStore::addFileInternal(const QString &path, QString *error)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        if (error)
            *error = QStringLiteral("文件不存在：%1").arg(path);
        return false;
    }
    if (!info.isFile()) {
        if (error)
            *error = QStringLiteral("不是一个文件：%1").arg(path);
        return false;
    }
    if (m_files.size() >= kMaxFiles) {
        if (error)
            *error = QStringLiteral("最多同时共享 %1 个文件，请先移除一些").arg(kMaxFiles);
        return false;
    }

    const QString key = pathKey(path);
    for (const FileEntry &entry : m_files) {
        if (pathKey(entry.path) == key) {
            if (error)
                *error = QStringLiteral("这个文件已经在共享列表里了");
            return false;
        }
    }

    FileEntry entry;
    entry.id = newId();
    entry.path = info.absoluteFilePath();
    entry.name = info.fileName();
    entry.size = info.size();
    entry.addedAt = QDateTime::currentDateTime();
    m_files.append(entry);
    return true;
}

bool ShareStore::addFile(const QString &path, QString *error)
{
    const bool ok = addFileInternal(path, error);
    if (ok)
        emit changed();
    return ok;
}

int ShareStore::addDirectory(const QString &dir, QString *error)
{
    const QFileInfo info(dir);
    if (!info.exists() || !info.isDir()) {
        if (error)
            *error = QStringLiteral("不是一个文件夹：%1").arg(dir);
        return 0;
    }

    int added = 0;
    int skippedFull = 0;
    // QDirIterator 默认不跟随符号链接，天然避免目录环（Linux 上软链回上级会死循环）。
    QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (m_files.size() >= kMaxFiles || added >= kMaxDirFiles) {
            ++skippedFull;
            break;
        }
        it.next();
        QString ignored;
        if (addFileInternal(it.filePath(), &ignored))
            ++added;
    }

    if (added > 0)
        emit changed();

    if (error) {
        if (added == 0 && skippedFull > 0)
            *error = QStringLiteral("共享列表已满（最多 %1 个文件）").arg(kMaxFiles);
        else if (added == 0)
            *error = QStringLiteral("这个文件夹里没有可共享的文件");
        else if (skippedFull > 0)
            *error = QStringLiteral("已加入 %1 个文件，达到上限后停止").arg(added);
        else
            error->clear();
    }
    return added;
}

bool ShareStore::removeFile(const QString &id)
{
    for (int i = 0; i < m_files.size(); ++i) {
        if (m_files.at(i).id != id)
            continue;

        const QString path = m_files.at(i).path;
        m_files.removeAt(i);

        // 只删本服务自己存的上传副本；用户硬盘上的原文件永远不动。
        if (isManagedUpload(path))
            QFile::remove(path);

        emit changed();
        return true;
    }
    return false;
}

void ShareStore::clearFiles()
{
    if (m_files.isEmpty())
        return;

    // 清空同样只回收上传副本：用户自己的文件被"共享列表"删掉会很吓人。
    for (const FileEntry &entry : m_files) {
        if (isManagedUpload(entry.path))
            QFile::remove(entry.path);
    }
    m_files.clear();
    emit changed();
}

const FileEntry *ShareStore::findFile(const QString &id) const
{
    for (const FileEntry &entry : m_files) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

bool ShareStore::isManagedUpload(const QString &path) const
{
    const QString dir = uploadDirPath();
    if (dir.isEmpty())
        return false;
    const QString target = pathKey(path);
    const QString prefix = dir + QLatin1Char('/');
#if defined(Q_OS_WIN)
    // Windows 文件系统不区分大小写；Linux 区分（用不敏感比较会误删同名不同大小写的
    // 用户文件）。macOS 默认不敏感但可配成敏感，这里按敏感处理 —— 最坏结果是留下一个
    // 上传副本没删掉，比删错用户文件安全得多。
    return target.startsWith(prefix, Qt::CaseInsensitive);
#else
    return target.startsWith(prefix, Qt::CaseSensitive);
#endif
}

QString ShareStore::uploadDir()
{
    const QString dir = uploadDirPath();
    if (dir.isEmpty())
        return QString();
    if (!QDir().mkpath(dir))
        return QString();
    return dir;
}

// ------------------------------------------------------------------ 文本

QString ShareStore::addText(const QString &text, const QString &from, QString *error)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("文本内容为空");
        return QString();
    }
    // 用 UTF-8 字节数而不是字符数：超长文本经 JSON 往返会撑大轮询响应，
    // 而中文一个字符占 3 字节，按字符限制会低估实际开销。
    const int bytes = trimmed.toUtf8().size();
    if (bytes > kMaxTextBytes) {
        if (error)
            *error = QStringLiteral("单条文本最多 %1 KB（当前 %2 KB）")
                         .arg(kMaxTextBytes / 1024)
                         .arg((bytes + 1023) / 1024);
        return QString();
    }

    TextEntry entry;
    entry.id = newId();
    entry.text = trimmed;
    entry.from = from.isEmpty() ? QStringLiteral("未知来源") : from;
    entry.createdAt = QDateTime::currentDateTime();

    m_texts.append(entry);
    // 超出上限丢最旧的：共享文本是"临时看一眼"的用途，不是归档，
    // 让新内容总能看到比留着一堆旧内容更有用（面板上也有清空按钮）。
    while (m_texts.size() > kMaxTexts)
        m_texts.removeFirst();

    emit changed();
    return entry.id;
}

bool ShareStore::removeText(const QString &id)
{
    for (int i = 0; i < m_texts.size(); ++i) {
        if (m_texts.at(i).id != id)
            continue;
        m_texts.removeAt(i);
        emit changed();
        return true;
    }
    return false;
}

void ShareStore::clearTexts()
{
    if (m_texts.isEmpty())
        return;
    m_texts.clear();
    emit changed();
}

const TextEntry *ShareStore::findText(const QString &id) const
{
    for (const TextEntry &entry : m_texts) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

} // namespace share

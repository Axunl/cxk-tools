#include "MemeCache.h"

#include "MemeSource.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace meme {

namespace {

/// outbox 子目录名。与缓存文件同层，但清理时整体跳过（见文件头说明）。
constexpr auto kOutboxDir = "outbox";

/// 允许的缓存扩展名，用于 find() 依次试探。
/// 与 ImageKind 一一对应，但不含 webp —— 运行时没有 qwebp 解码插件，
/// 落盘后下次只会"命中一个打不开的文件"，不如每次明确提示不支持。
const char *const kCacheExtensions[] = {"gif", "jpg", "png", "bmp"};

} // namespace

Cache::Cache()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) {
        // 极少数精简环境（无 XDG 配置的容器）拿不到 CacheLocation，
        // 退到 AppLocalDataLocation 下的 cache/，语义相同、同样不进漫游。
        const QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        base = data.isEmpty() ? QDir::tempPath() : data + QStringLiteral("/cache");
    }
    m_root = QDir(base).filePath(QStringLiteral("memes"));
    QDir().mkpath(m_root);
}

QString Cache::find(const QString &key) const
{
    if (key.isEmpty())
        return QString();

    for (const char *ext : kCacheExtensions) {
        const QString path =
            QDir(m_root).filePath(key + QLatin1Char('.') + QLatin1String(ext));
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile())
            continue;
        if (info.size() <= 0) {
            // 上次写到一半留下的空壳：删掉当未命中，别让面板对着一张空图反复解码。
            QFile::remove(path);
            continue;
        }
        return path;
    }
    return QString();
}

QString Cache::store(const QString &key, const QByteArray &bytes, QString *error) const
{
    if (key.isEmpty() || bytes.isEmpty()) {
        if (error)
            *error = QStringLiteral("空数据，未缓存");
        return QString();
    }

    const ImageKind kind = sniffImageKind(bytes);
    if (kind == ImageKind::Unknown) {
        if (error)
            *error = QStringLiteral("无法识别的图片格式（可能是错误页或防盗链拦截）");
        return QString();
    }
    if (!isSupportedKind(kind)) {
        if (error)
            *error = QStringLiteral("WebP 图片暂不支持（运行时没有 WebP 解码插件）");
        return QString();
    }

    const QString path =
        QDir(m_root).filePath(key + QLatin1Char('.') + extensionFor(kind));
    const QFileInfo existing(path);
    if (existing.exists() && existing.size() == bytes.size())
        return path;

    // QSaveFile：先写临时文件再原子替换，插件崩了也不会留下半张图。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return QString();
    }
    if (file.write(bytes) != bytes.size()) {
        const QString reason = file.errorString();
        file.cancelWriting();
        if (error)
            *error = reason;
        return QString();
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return QString();
    }
    return path;
}

QString Cache::outboxFile(const QString &key, const QString &sourcePath) const
{
    if (key.isEmpty() || sourcePath.isEmpty())
        return QString();

    const QFileInfo source(sourcePath);
    if (!source.exists() || source.size() <= 0)
        return QString();

    const QString dir = QDir(m_root).filePath(QLatin1String(kOutboxDir));
    if (!QDir().mkpath(dir))
        return QString();

    const QString target = QDir(dir).filePath(source.fileName());
    const QFileInfo existing(target);
    if (existing.exists() && existing.size() == source.size())
        return target;

    QFile::remove(target);   // 大小不一致视为旧副本，先删再拷（QFile::copy 不覆盖）
    if (!QFile::copy(sourcePath, target))
        return QString();
    return target;
}

void Cache::prune(int maxFiles, qint64 maxBytes) const
{
    QDir dir(m_root);
    if (!dir.exists())
        return;

    // 只取根目录下的普通文件：outbox/ 是目录，自然被过滤掉。
    const QFileInfoList files =
        dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Time | QDir::Reversed);
    // QDir::Time 是「最新在前」，Reversed 之后最旧的排在最前面 —— 正好先删。

    QList<QFileInfo> victims;
    qint64 total = 0;
    for (const QFileInfo &info : files)
        total += info.size();

    int remaining = files.size();
    for (const QFileInfo &info : files) {
        if (remaining <= maxFiles && total <= maxBytes)
            break;
        victims.append(info);
        total -= info.size();
        --remaining;
    }

    for (const QFileInfo &info : victims)
        QFile::remove(info.absoluteFilePath());
}

} // namespace meme

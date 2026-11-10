#pragma once

// ---------------------------------------------------------------------------
// 表情包图片的磁盘缓存。
//
// 为什么必须有缓存，而不只是"优化"：点一下就把图片放进剪贴板，走的是
// 「文件通道」（QUrl -> CF_HDROP / public.file-url），剪贴板里存的是**路径**。
// 如果图片只活在内存里，粘贴时目标程序读不到任何东西。
//
// 目录：QStandardPaths::CacheLocation/memes（可被系统清理，语义正确）。
//   Windows  %LOCALAPPDATA%\cxk\cxk-tools\cache\memes
//   macOS    ~/Library/Caches/cxk/cxk-tools/memes
//   Linux    ~/.cache/cxk/cxk-tools/memes
//
// 文件名为图片 URL 的 SHA-256 + 嗅探出的扩展名；扩展名来自下载后的字节，
// 不信 URL 后缀（图床把 gif 挂成 .jpg 很常见）。
//
// outbox/ 子目录单独存放"已复制到剪贴板"的那一份，**不参与容量清理**：
// 剪贴板里的路径可能几分钟后才被粘贴，那时缓存若已被清掉就是一次静默失败。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QString>

namespace meme {

class Cache
{
public:
    Cache();

    /// 缓存根目录（构造时已 mkpath）。
    QString root() const { return m_root; }

    /// 按 key 找已缓存的图片路径。命中要求文件存在且大小 > 0；零字节文件会被删掉当未命中。
    QString find(const QString &key) const;

    /// 写入缓存并返回最终路径。失败返回空并写 error。
    /// 用 QSaveFile 提交：进程被杀/断电时不会留下半张图，下次也不会把它当有效缓存。
    QString store(const QString &key, const QByteArray &bytes, QString *error) const;

    /// 给剪贴板用的稳定副本：把 sourcePath 复制到 outbox/，返回副本路径。
    /// 已存在且大小一致时直接复用，避免每次点击都多写一份。
    QString outboxFile(const QString &key, const QString &sourcePath) const;

    /// 按「最久未修改优先」清理，直到文件数 <= maxFiles 且总字节 <= maxBytes。
    /// 只扫根目录下的普通文件，不递归，也绝不碰 outbox/。
    /// 面板每写入若干张就跑一次，容量上限见 MemePanel.cpp 的 kCacheMax*，
    /// 不给「清空缓存」按钮：缓存落在系统缓存目录，要清直接删目录即可。
    void prune(int maxFiles, qint64 maxBytes) const;

private:
    QString m_root;
};

} // namespace meme

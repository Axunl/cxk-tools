#pragma once

// ---------------------------------------------------------------------------
// 文件共享的数据模型：一份「共享文件」列表 + 一份「共享文本」列表。
//
// 为什么是进程级单例而不是面板成员：共享出去的地址是**一个端口**，网页端看到的
// 必须是同一份数据；而宿主允许同一工具同时存在「内嵌面板」与「独立窗口面板」两件
// （AGENTS.md 交互约定第 5 条）。若把列表放进面板，两份面板会各有一份数据、
// 桌面端加了文件而网页端看不到。所以模型与服务器一起放在插件模块的进程级单例里，
// 面板只当它的视图 —— 这是对「别用静态/全局状态」的**有意破例**，理由是不可协商的
// 端口独占与数据一致性。
// ---------------------------------------------------------------------------

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

namespace share {

/// 一条共享文件。只**引用**本地已有文件，不复制内容：
/// 用户把源文件删掉后，列表项还在但会标成「已丢失」，下载返回 404。
struct FileEntry
{
    QString   id;       ///< 短 id，出现在 URL 里（/f/<id>）
    QString   path;     ///< 本地绝对路径
    QString   name;     ///< 展示名 = 文件名
    qint64    size = 0; ///< 加入时的字节数（之后文件变化不追踪）
    QDateTime addedAt;  ///< 加入时间（本地时区）
};

/// 一条共享文本。from 记录来源：桌面端发的写「本机」，网页端发的写对方 IP。
struct TextEntry
{
    QString   id;
    QString   text;
    QString   from;
    QDateTime createdAt;
};

class ShareStore : public QObject
{
    Q_OBJECT

public:
    /// 进程内唯一实例。刻意不析构（见 .cpp 里的说明）。
    static ShareStore &instance();

    const QList<FileEntry> &files() const { return m_files; }
    const QList<TextEntry> &texts() const { return m_texts; }

    /// 加入一个文件。失败时把原因写进 error（不存在 / 不是文件 / 已在列表里 / 超上限）。
    bool addFile(const QString &path, QString *error);

    /// 递归收录一个目录下的所有文件（跳过符号链接，避免环）。
    /// 返回新增个数；error 只在"一个都没加进去"时填，部分成功仍返回个数。
    int addDirectory(const QString &dir, QString *error);

    /// 移出共享列表。若该文件是本服务上传目录里的副本，同时删掉磁盘文件，
    /// 避免上传目录无限增长；用户自己的文件绝不会被删。
    bool removeFile(const QString &id);
    void clearFiles();

    /// 追加一条文本。空文本、超长文本直接拒绝（error 给原因）。
    /// 超过条数上限时丢弃最旧的一条，并返回新条目 id。
    QString addText(const QString &text, const QString &from, QString *error);
    bool removeText(const QString &id);
    void clearTexts();

    const FileEntry *findFile(const QString &id) const;
    const TextEntry *findText(const QString &id) const;

    /// 网页上传的落盘目录（首次调用时创建）。失败返回空串。
    QString uploadDir();

    // ---- 上限。都写在类里而不是散落在各处，改一个地方即可 --------------------
    static constexpr int    kMaxFiles     = 200;             ///< 共享文件条数上限
    static constexpr int    kMaxTexts     = 50;              ///< 共享文本条数上限
    static constexpr int    kMaxTextBytes = 16 * 1024;       ///< 单条文本字节上限
    static constexpr int    kMaxDirFiles  = 300;             ///< 一次收录目录的文件上限
    static constexpr qint64 kMaxUpload    = 2ll * 1024 * 1024 * 1024; ///< 单次上传上限 2 GB

signals:
    /// 列表有任何变化（增删清空）。面板据此重建视图，服务器不需要监听。
    void changed();

private:
    explicit ShareStore(QObject *parent = nullptr);

    /// 生成一个不与现有条目冲突的短 id（12 位十六进制）。
    QString newId() const;

    /// 加入文件但不发 changed()：批量收录目录时只在最后通知一次，
    /// 否则面板会按文件数重建 N 次视图（几百个文件时肉眼可见地卡）。
    bool addFileInternal(const QString &path, QString *error);

    /// path 是否落在上传目录内（判断能否连带删除磁盘文件）。
    bool isManagedUpload(const QString &path) const;

    QList<FileEntry> m_files;
    QList<TextEntry> m_texts;
};

} // namespace share

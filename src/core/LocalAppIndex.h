#pragma once

// ---------------------------------------------------------------------------
// 本机应用索引：把系统里「用户能点开的应用入口」列出来，供首页搜索使用。
//
// 每个平台只索引一种稳定入口，不解析安装信息：
//   Windows  → 开始菜单里的 .lnk（用户 + 全局两处，递归子目录）
//   macOS    → /Applications、/System/Applications 及用户目录下的 .app 包
//   Linux    → XDG applications 目录下的 .desktop（解析 Name/Exec/Icon）
// 启动统一交给系统：Windows/macOS 用 QDesktopServices（ShellExecute / LaunchServices），
// Linux 优先 `gio launch`，失败再按 Exec 起进程。
//
// 为什么 id 是哈希而不是路径：最近使用记录要写进 QSettings 的 INI，
// 键里带 '/' 会被当成组层级、带盘符冒号也不好读。id 取路径的 FNV-1a 64 位哈希
// （app:<16 hex>），既稳定又天然避免非法字符；路径没变则 id 一直有效。
//
// 扫描是同步的：开始菜单通常只有几百个 .lnk，遍历目录是毫秒级；图标不做预取，
// 由 icon() 按需读取（真正慢的是系统图标查询）。
// ---------------------------------------------------------------------------

#include <QFileIconProvider>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class LocalAppIndex : public QObject
{
    Q_OBJECT

public:
    struct Entry
    {
        QString id;        ///< app:<16 hex>，全局唯一键（最近使用/使用频次都用它）
        QString name;      ///< 展示名（Windows 去掉 " - 快捷方式" 后缀）
        QString path;      ///< 入口文件路径（.lnk / .app / .desktop）
        QString exec;      ///< 仅 Linux：去掉 %f/%U 等字段码后的启动命令
        QString iconName;  ///< 仅 Linux：desktop 文件里的 Icon= 值（主题名或绝对路径）
    };

    explicit LocalAppIndex(QObject *parent = nullptr);

    /// 重新扫描并替换索引。启动时调用一次；运行期新装的应用重启后出现。
    void scan();

    int count() const { return static_cast<int>(m_entries.size()); }
    const QList<Entry> &entries() const { return m_entries; }
    const Entry *entryById(const QString &id) const;

    /// 按名称匹配，返回按相关度排序的 id。空查询直接返回空（应用不铺满首页）。
    QStringList search(const QString &query, int limit = 24) const;

    /// 按需取图标（系统图标查询较慢，取到后缓存）。
    QIcon icon(const QString &id) const;

    /// 启动应用。失败时把原因写进 error（会显示给用户，不要留英文堆栈）。
    bool launch(const QString &id, QString *error = nullptr) const;

    /// 在文件管理器里显示入口所在目录。
    void reveal(const QString &id) const;

    /// 路径 → 稳定 id。同一路径（Windows 下大小写不敏感）永远得到同一个 id。
    static QString idForPath(const QString &path);

    /// 是否为应用 id。工具 id 由插件自定义，保留 "app:" 前缀给本机应用。
    static bool isAppId(const QString &id);

signals:
    void changed();

private:
    void scanWindows();
    void scanMac();
    void scanLinux();
    void addEntry(const QString &name, const QString &path,
                  const QString &exec = QString(), const QString &iconName = QString());

    QList<Entry> m_entries;
    mutable QHash<QString, QIcon> m_icons;   ///< 按需生成的图标缓存
    mutable QFileIconProvider m_provider;    ///< 系统图标来源（icon() 是 const，故 mutable）
};

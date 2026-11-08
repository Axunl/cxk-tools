#pragma once

// ---------------------------------------------------------------------------
// 工具注册表：保存所有已就绪的工具，并提供搜索匹配。
//
// 与 PluginManager 的分工：PluginManager 负责「把动态库变成 ToolEntry」，
// ToolRegistry 只负责「存与查」，不碰加载。因此搜索逻辑可以脱离插件机制单独测试。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QIcon>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

/// 一条已就绪的工具记录（内置或插件）。
struct ToolEntry
{
    xtools::ToolInfo info;
    QIcon            icon;         ///< 有自带图标用它，否则是生成的字母徽章
    xtools::ITool   *tool = nullptr;   ///< 生命周期归 PluginManager 的 QPluginLoader
    QString          pluginFile;   ///< 来源动态库路径，内置工具为空
    bool             builtin = false;

    QString id() const { return info.id; }
    QString name() const { return info.name; }
};

///
/// 工具注册表 + 搜索匹配。
///
/// 匹配规则（分数越高越靠前，最高分胜出，同分再用使用频次加成）：
///   名称完全相等 110 / 名称前缀 100 / 关键词完全相等 95 / 名称包含 85 /
///   关键词前缀 75 / 关键词包含 65 / id 包含 60 / 描述包含 30。
/// 都不命中时再试一次"首字母缩写"（如 jf -> JSON 格式化），命中记 40。
///
class ToolRegistry : public QObject
{
    Q_OBJECT

public:
    explicit ToolRegistry(QObject *parent = nullptr);

    /// 加入一个工具。id 为空或 tool 为空、id 重复的条目会被忽略并记警告。
    void add(const ToolEntry &entry);
    void clear();

    int count() const { return static_cast<int>(m_entries.size()); }
    const QList<ToolEntry> &entries() const { return m_entries; }

    const ToolEntry *entryById(const QString &id) const;

    /// 返回按相关度排序的工具 id。query 为空时按使用频次/名称排序。
    /// @param limit 最多返回多少条（首页搜索过滤时传大值即可）
    QStringList search(const QString &query, int limit = 9) const;

signals:
    void changed();

private:
    QList<ToolEntry> m_entries;
};

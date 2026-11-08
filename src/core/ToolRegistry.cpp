#include "ToolRegistry.h"

#include "Settings.h"

#include <QDebug>
#include <algorithm>

namespace {

/// 单条工具与查询的匹配分。0 表示不匹配。
///
/// 分值设计意图：名称命中 > 关键词命中 > id/描述命中。
/// 名称前缀 100 与名称相等 110 拉开 10 分，保证「手输全名」永远排第一。
int matchScore(const ToolEntry &entry, const QString &q)
{
    const QString name = entry.info.name.toLower();
    const QString id = entry.info.id.toLower();
    const QString desc = entry.info.description.toLower();

    int best = 0;
    if (name == q)                    best = 110;
    else if (name.startsWith(q))      best = 100;
    else if (name.contains(q))        best = 85;

    for (const QString &kw : entry.info.keywords) {
        const QString k = kw.toLower();
        if (k == q)                   best = std::max(best, 95);
        else if (k.startsWith(q))     best = std::max(best, 75);
        else if (k.contains(q))       best = std::max(best, 65);
    }

    if (id.contains(q))               best = std::max(best, 60);
    if (desc.contains(q))             best = std::max(best, 30);
    return best;
}

/// "首字母缩写"匹配：把关键词与名称按空格切词，各取首字母拼成串，
/// 查询是它的子串就算命中（"jf" -> "json format" -> "jf"）。
///
/// 这是给中文/英文混合名称兜底的廉价方案，比拼音简单得多，也够用。
int initialsScore(const ToolEntry &entry, const QString &q)
{
    QString initials;
    for (const QString &kw : entry.info.keywords) {
        for (const QString &word : kw.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (!word.isEmpty())
                initials.append(word.at(0).toLower());
        }
    }
    for (const QString &word : entry.info.name.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!word.isEmpty())
            initials.append(word.at(0).toLower());
    }
    return initials.contains(q) ? 40 : 0;
}

} // namespace

ToolRegistry::ToolRegistry(QObject *parent)
    : QObject(parent)
{
}

void ToolRegistry::add(const ToolEntry &entry)
{
    // 防御：没有 id 就没法做稳定引用（配置里的最近使用、使用频次都靠它）
    if (entry.id().isEmpty() || !entry.tool) {
        qWarning() << "ToolRegistry: 忽略无效工具条目" << entry.info.name;
        return;
    }
    if (entryById(entry.id())) {
        // 重复 id 通常意味着两个插件目录里有同名插件，或者插件忘改 id
        qWarning() << "ToolRegistry: 工具 id 重复，已忽略" << entry.id();
        return;
    }
    m_entries.append(entry);
    emit changed();
}

void ToolRegistry::clear()
{
    m_entries.clear();
    emit changed();
}

const ToolEntry *ToolRegistry::entryById(const QString &id) const
{
    for (const ToolEntry &e : m_entries) {
        if (e.info.id == id)
            return &e;
    }
    return nullptr;
}

QStringList ToolRegistry::search(const QString &query, int limit) const
{
    const QString q = query.trimmed().toLower();

    struct Scored
    {
        QString id;
        int score;
    };
    QList<Scored> hits;
    hits.reserve(m_entries.size());

    for (const ToolEntry &e : m_entries) {
        const int usage = Settings::instance().usage(e.info.id);

        if (q.isEmpty()) {
            // 空查询 = 默认列表：用得多的靠前（上限 500 防止某个工具永远霸榜）
            hits.append({e.info.id, 1000 - std::min(usage, 500)});
            continue;
        }

        int s = matchScore(e, q);
        if (q.size() > 1 && s == 0)
            s = initialsScore(e, q);
        if (s > 0)
            hits.append({e.info.id, s + std::min(usage, 20)});   // 频次加成封顶 20，压不过一级匹配
    }

    std::sort(hits.begin(), hits.end(), [this](const Scored &a, const Scored &b) {
        if (a.score != b.score)
            return a.score > b.score;
        // 同分时按名称排序，保证结果稳定（否则会随加载顺序抖动）
        const ToolEntry *ea = entryById(a.id);
        const ToolEntry *eb = entryById(b.id);
        return ea && eb ? ea->info.name.localeAwareCompare(eb->info.name) < 0 : a.id < b.id;
    });

    QStringList ids;
    ids.reserve(qMin(limit, static_cast<int>(hits.size())));
    for (int i = 0; i < hits.size() && i < limit; ++i)
        ids.append(hits.at(i).id);
    return ids;
}

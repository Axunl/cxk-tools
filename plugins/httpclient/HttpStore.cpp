#include "HttpStore.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace httpapi {
namespace {

/// 存档版本。字段结构变了就 +1；读的时候不认识的版本按空档处理（宁可丢存档，
/// 也不要把旧字段塞进新结构里造成「看起来有、其实全是默认值」）。
constexpr int kStoreVersion = 1;

/// 默认落盘位置。AppConfigLocation 在三个平台都指向「用户配置目录 + 应用名」，
/// 与 jsrun 的 jsrun.ini 同一层，卸载/清理时能一起被找到。
QString defaultPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/httpclient.json");
}

} // namespace

Store::Store(QString path)
    : m_path(path.isEmpty() ? defaultPath() : std::move(path))
{
}

QJsonObject Store::load() const
{
    QFile file(m_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return QJsonObject();

    const QByteArray raw = file.readAll();
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        // 坏档不中断工具：下一次保存会用新内容整份覆盖。这里只在日志里留痕，
        // 方便用户反馈「我的集合没了」时能查到是解析失败而不是写丢了。
        qWarning() << "httpclient: 存档解析失败，" << m_path << error.errorString();
        return QJsonObject();
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt() != kStoreVersion)
        return QJsonObject();
    return root;
}

void Store::save(const QJsonObject &root) const
{
    const QString dir = QFileInfo(m_path).absolutePath();
    QDir().mkpath(dir);   // 首次运行/用户清过配置目录时不存在，QSaveFile 不会代建

    // QSaveFile 先写临时文件再原子替换：面板在频繁保存草稿，进程被强杀时
    // 也不会留下半截 JSON（下次启动就会判成坏档，用户的集合全丢）。
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "httpclient: 无法写入存档" << m_path << file.errorString();
        return;
    }
    QJsonObject out = root;
    out.insert(QStringLiteral("version"), kStoreVersion);
    file.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
    if (!file.commit())
        qWarning() << "httpclient: 存档提交失败" << m_path << file.errorString();
}

Spec Store::draft() const
{
    const QJsonObject root = load();
    if (root.isEmpty())
        return Spec();
    return specFromJson(root.value(QStringLiteral("draft")).toObject());
}

void Store::setDraft(const Spec &spec)
{
    QJsonObject root = load();
    root.insert(QStringLiteral("draft"), specToJson(spec));
    save(root);
}

QVector<Spec> Store::saved() const
{
    QVector<Spec> items;
    const QJsonArray array = load().value(QStringLiteral("saved")).toArray();
    for (const QJsonValue &value : array) {
        Spec spec = specFromJson(value.toObject());
        if (spec.url.trimmed().isEmpty())
            continue;
        if (spec.name.trimmed().isEmpty())
            spec.name = spec.url;
        items.append(spec);
    }
    return items;
}

void Store::setSaved(const QVector<Spec> &items)
{
    QJsonObject root = load();
    QJsonArray array;
    for (const Spec &spec : items) {
        if (spec.url.trimmed().isEmpty())
            continue;
        array.append(specToJson(spec));
    }
    root.insert(QStringLiteral("saved"), array);
    save(root);
}

QVector<HistoryItem> Store::history() const
{
    QVector<HistoryItem> items;
    const QJsonArray array = load().value(QStringLiteral("history")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        HistoryItem item;
        item.spec = specFromJson(object.value(QStringLiteral("spec")).toObject());
        if (item.spec.url.trimmed().isEmpty())
            continue;
        item.at = QDateTime::fromString(object.value(QStringLiteral("at")).toString(), Qt::ISODate);
        items.append(item);
    }
    return items;
}

void Store::setHistory(const QVector<HistoryItem> &items)
{
    QJsonObject root = load();
    QJsonArray array;
    for (const HistoryItem &item : items) {
        if (item.spec.url.trimmed().isEmpty())
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("at"), item.at.toString(Qt::ISODate));
        object.insert(QStringLiteral("spec"), specToJson(item.spec));
        array.append(object);
    }
    root.insert(QStringLiteral("history"), array);
    save(root);
}

void Store::addHistory(const Spec &spec, int max)
{
    if (spec.url.trimmed().isEmpty())
        return;

    QVector<HistoryItem> items = history();

    // 同方法 + 同地址 + 同请求体视为同一条：重复调同一个接口不应该把历史刷满。
    // 地址比较用 effectiveUrl，这样「参数表顺序不同但请求一样」也能去重。
    const QString keyUrl = effectiveUrl(spec);
    for (int i = items.size() - 1; i >= 0; --i) {
        const Spec &old = items.at(i).spec;
        if (old.method.compare(spec.method, Qt::CaseInsensitive) == 0
            && effectiveUrl(old) == keyUrl && old.body == spec.body) {
            items.removeAt(i);
        }
    }

    HistoryItem item;
    item.spec = spec;
    item.spec.name.clear();   // 历史不承担命名职责，名字只属于集合
    item.at = QDateTime::currentDateTime();
    items.prepend(item);

    while (items.size() > qMax(1, max))
        items.removeLast();
    setHistory(items);
}

} // namespace httpapi

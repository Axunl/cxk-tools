#include "FrpcStore.h"

#include "FrpcText.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace frpc {
namespace {

/// 存档版本。字段结构变了就 +1，旧档按空处理 —— 与 httpclient 的做法一致：
/// 宁可让用户重新填一次，也不要把旧字段硬塞进新结构，造成「看起来有、其实全是空」。
constexpr int kStoreVersion = 1;

QString defaultPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir + QStringLiteral("/frpc.json");
}

} // namespace

Store::Store(QString path)
    : m_path(path.isEmpty() ? defaultPath() : std::move(path))
{
}

QString Store::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVector<Profile> Store::profiles() const
{
    QVector<Profile> items;

    QFile file(m_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return items;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        // 坏档只在日志里留痕，不打断工具：下一次保存会整份覆盖。
        qWarning() << "frpc: 存档解析失败" << m_path << error.errorString();
        return items;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt() != kStoreVersion)
        return items;

    const QJsonArray array = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        Profile profile;
        profile.id = object.value(QStringLiteral("id")).toString();
        profile.name = object.value(QStringLiteral("name")).toString();
        profile.binPath = object.value(QStringLiteral("binPath")).toString();
        profile.config = object.value(QStringLiteral("config")).toString();

        // 缺 id 的条目直接补齐而不是丢弃：用户手改过存档时，配置文本是最贵重的东西。
        if (profile.id.isEmpty())
            profile.id = newId();
        if (profile.name.trimmed().isEmpty())
            profile.name = QStringLiteral("未命名配置");
        items.append(profile);
    }

    return items;
}

void Store::setProfiles(const QVector<Profile> &profiles)
{
    // 读旧档拿 activeIndex，再整份覆盖：只改 profiles 不该把「上次选的是哪一档」抹掉。
    QJsonObject root;
    if (QFile::exists(m_path)) {
        QFile file(m_path);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (doc.isObject())
                root = doc.object();
        }
    }

    QJsonArray array;
    for (const Profile &profile : profiles) {
        if (profile.id.isEmpty())
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("id"), profile.id);
        object.insert(QStringLiteral("name"), profile.name);
        object.insert(QStringLiteral("binPath"), profile.binPath);
        object.insert(QStringLiteral("config"), profile.config);
        array.append(object);
    }
    root.insert(QStringLiteral("profiles"), array);
    root.insert(QStringLiteral("version"), kStoreVersion);

    // QSaveFile 先写临时文件再原子替换：面板在频繁自动保存，强杀时不能留下半截 JSON。
    const QString dir = QFileInfo(m_path).absolutePath();
    QDir().mkpath(dir);   // QSaveFile 不会代建父目录，首次运行时会静默失败

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "frpc: 无法写入存档" << m_path << file.errorString();
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit())
        qWarning() << "frpc: 存档提交失败" << m_path << file.errorString();
}

int Store::activeIndex() const
{
    QFile file(m_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return 0;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return 0;
    return doc.object().value(QStringLiteral("activeIndex")).toInt(0);
}

void Store::setActiveIndex(int index)
{
    QFile file(m_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return;

    QJsonObject root = doc.object();
    root.insert(QStringLiteral("activeIndex"), index);

    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly))
        return;
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.commit();
}

QVector<Profile> Store::ensureDefaults()
{
    QVector<Profile> items = profiles();
    if (!items.isEmpty())
        return items;

    Profile profile;
    profile.id = newId();
    profile.name = QStringLiteral("默认配置");
    profile.config = frpctext::defaultConfig();
    items.append(profile);

    setProfiles(items);
    setActiveIndex(0);
    return items;
}

} // namespace frpc

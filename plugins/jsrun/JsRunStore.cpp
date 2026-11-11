#include "JsRunStore.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <utility>

namespace jsrun {

QString iniPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    // 首次运行或用户清过配置目录时这里不存在，写之前必须先建出来，
    // 否则 QSettings 会静默失败（它不报错，只是什么都没写进去）。
    QDir().mkpath(dir);
    return dir + QStringLiteral("/jsrun.ini");
}

Store::Store(QString path)
    : m_path(std::move(path))
{
}

QString Store::lastScript() const
{
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    return settings.value(QStringLiteral("general/last_script")).toString();
}

void Store::setLastScript(const QString &code)
{
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    settings.setValue(QStringLiteral("general/last_script"), code);
}

QStringList Store::history() const
{
    QSettings settings(m_path, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);

    QStringList items;
    const int count = settings.beginReadArray(QStringLiteral("history"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString code = settings.value(QStringLiteral("code")).toString();
        if (!code.trimmed().isEmpty())
            items.append(code);
    }
    settings.endArray();
    return items;
}

void Store::addHistory(const QString &code)
{
    if (code.trimmed().isEmpty())
        return;

    QStringList items = history();
    // 同一个片段再次运行只是把它提到最前，不留重复项
    items.removeAll(code);
    items.prepend(code);
    while (items.size() > kMaxHistory)
        items.removeLast();

    QSettings settings(m_path, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    // 整体重写而不是逐项覆盖：否则条目变少时旧的 history/N 会残留，
    // 下次读出来就是尾部脏数据。
    settings.remove(QStringLiteral("history"));
    settings.beginWriteArray(QStringLiteral("history"), items.size());
    for (int i = 0; i < items.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("code"), items.at(i));
    }
    settings.endArray();
}

} // namespace jsrun

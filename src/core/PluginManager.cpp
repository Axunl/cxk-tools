#include "PluginManager.h"

#include "IconBadge.h"
#include "ToolRegistry.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLibrary>
#include <QPluginLoader>

PluginManager::PluginManager(ToolRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
}

PluginManager::~PluginManager()
{
    unloadAll();
}

QString PluginManager::pluginDir() const
{
    // 固定为「可执行文件同级/plugins」：Windows 是 exe 旁边，macOS 是
    // xxx.app/Contents/MacOS/plugins。三平台一致，不依赖安装目录结构。
    return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
}

void PluginManager::loadAll()
{
    const QString dirPath = pluginDir();
    QDir dir(dirPath);

    if (!dir.exists()) {
        qWarning() << "PluginManager: 插件目录不存在:" << dirPath;
        dir.mkpath(QStringLiteral("."));
        return;
    }

    // 用 QLibrary::isLibrary() 判断而不是写死 "*.dll"：
    // Windows .dll / macOS .dylib / Linux .so 一套代码通吃。
    // 顺带避免把插件目录里的图标、说明文档当成插件尝试加载。
    const QFileInfoList candidates = dir.entryInfoList(QDir::Files, QDir::Name);
    QFileInfoList files;
    for (const QFileInfo &fi : candidates) {
        if (QLibrary::isLibrary(fi.absoluteFilePath()))
            files.append(fi);
    }
    qInfo() << "PluginManager: 扫描" << dirPath << "发现" << files.size() << "个候选文件";

    for (const QFileInfo &fi : files) {
        auto *loader = new QPluginLoader(fi.absoluteFilePath(), this);

        QObject *instance = loader->instance();
        if (!instance) {
            const QString err = QStringLiteral("加载 %1 失败：%2").arg(fi.fileName(), loader->errorString());
            qWarning() << "PluginManager:" << err;
            m_errors.append(err);
            loader->deleteLater();
            continue;
        }

        auto *tool = qobject_cast<xtools::ITool *>(instance);
        if (!tool) {
            // 目录里混进了别的 Qt 插件（比如某个 DLL 只是碰巧符合 isLibrary）
            const QString err = QStringLiteral("%1 不是有效的工具插件（缺少 Q_PLUGIN_METADATA/IID）")
                                    .arg(fi.fileName());
            qWarning() << "PluginManager:" << err;
            m_errors.append(err);
            loader->unload();
            loader->deleteLater();
            continue;
        }

        // 版本挡板：宁可拒绝加载，也不要在虚表布局不一致时崩溃。
        // 这类崩溃通常发生在调用 info() 的第一行，堆栈毫无意义，排查成本极高。
        if (tool->apiVersion() != XTOOLS_API_VERSION) {
            const QString err = QStringLiteral("%1 的接口版本为 %2，当前宿主只支持 %3，请重新编译")
                                    .arg(fi.fileName())
                                    .arg(tool->apiVersion())
                                    .arg(XTOOLS_API_VERSION);
            qWarning() << "PluginManager:" << err;
            m_errors.append(err);
            loader->unload();
            loader->deleteLater();
            continue;
        }

        ToolEntry entry;
        entry.info = tool->info();
        entry.tool = tool;
        entry.pluginFile = fi.absoluteFilePath();
        entry.builtin = false;

        // 元信息缺失时兜底，尽量让插件仍然可用，同时在日志里留痕方便作者修正
        if (entry.info.id.isEmpty()) {
            entry.info.id = fi.completeBaseName().toLower();
            qWarning() << "PluginManager: 插件未提供 id，使用文件名:" << entry.info.id;
        }
        if (entry.info.name.isEmpty())
            entry.info.name = entry.info.id;

        // 图标：优先用插件目录下的相对路径文件，找不到就生成字母徽章
        if (!entry.info.iconPath.isEmpty()) {
            const QString iconFile = QDir(fi.absolutePath()).absoluteFilePath(entry.info.iconPath);
            if (QFileInfo::exists(iconFile)) {
                entry.icon = QIcon(iconFile);
            } else {
                qWarning() << "PluginManager: 图标文件不存在，改用字母徽章:" << iconFile;
            }
        }
        if (entry.icon.isNull())
            entry.icon = IconBadge::icon(entry.info.id, entry.info.name);

        m_registry->add(entry);
        m_loaders.append(loader);
        ++m_loadedCount;
        qInfo() << "PluginManager: 已加载工具" << entry.info.id << entry.info.name << "from" << fi.fileName();
    }

    qInfo() << "PluginManager: 加载完成，成功" << m_loadedCount << "失败" << m_errors.size();
}

void PluginManager::unloadAll()
{
    // 先 unload 再 deleteLater：析构时若插件仍处于 loaded 状态，
    // QPluginLoader 也会尝试卸载，重复卸载在某些平台上会告警。
    for (QPluginLoader *loader : std::as_const(m_loaders)) {
        if (loader->isLoaded())
            loader->unload();
        loader->deleteLater();
    }
    m_loaders.clear();
}

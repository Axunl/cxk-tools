#pragma once

// ---------------------------------------------------------------------------
// 插件加载器。
//
// 职责：扫描「可执行文件同级 /plugins」目录，把每个动态库变成一条 ToolEntry
// 交给 ToolRegistry；失败的只记录原因，绝不阻塞主程序启动。
//
// 不支持运行时热重载：unload 之后已创建的面板会指向失效代码段，
// 因此「重新扫描」的实现方式是重启应用（见 TrayController/SettingsPage）。
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>
#include <QStringList>

class ToolRegistry;
class QPluginLoader;

class PluginManager : public QObject
{
    Q_OBJECT

public:
    explicit PluginManager(ToolRegistry *registry, QObject *parent = nullptr);
    ~PluginManager() override;

    /// 扫描并注册全部插件。只应调用一次（重复调用会产生重复 id 警告）。
    void loadAll();

    /// 插件目录绝对路径（不存在时会尝试创建）。设置页要展示它。
    QString pluginDir() const;

    /// 加载失败的原因（用户可读），设置页逐条显示。
    QStringList errors() const { return m_errors; }
    int loadedCount() const { return m_loadedCount; }

private:
    void unloadAll();

    ToolRegistry *m_registry = nullptr;
    QList<QPluginLoader *> m_loaders;   ///< 持有 loader，保证插件实例在进程内有效
    QStringList m_errors;
    int m_loadedCount = 0;
};

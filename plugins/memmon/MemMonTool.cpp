#include "MemMonTool.h"

#include "MemMonPanel.h"

xtools::ToolInfo MemMonPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("memmon");
    info.name = QStringLiteral("内存监控");
    // 关键词覆盖中英文与常见缩写：搜 "mem" / "内存" / "任务管理器" 都能命中
    info.keywords = QStringList{QStringLiteral("mem"), QStringLiteral("memory"),
                               QStringLiteral("内存"), QStringLiteral("内存监控"),
                               QStringLiteral("进程"), QStringLiteral("process"),
                               QStringLiteral("任务管理器")};
    info.description = QStringLiteral("查看进程内存占用，随时释放内存");
    return info;
}

QWidget *MemMonPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都新建面板：宿主不缓存面板，状态（自动释放开关、已释放累计）
    // 也就随面板一起重置，符合「离开即清空」的既有约定
    return new MemMonPanel(parent);
}

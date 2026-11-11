#include "ProcMonTool.h"

#include "ProcMonPanel.h"

xtools::ToolInfo ProcMonPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("procmon");
    info.name = QStringLiteral("进程监控");
    // 关键词刻意把「内存」与「进程」两条都放进来：老用户照样搜"内存"找得到，
    // 想找"谁占着 8080"的人搜"端口"也能命中首页这一格
    info.keywords = QStringList{QStringLiteral("proc"), QStringLiteral("process"),
                               QStringLiteral("进程"), QStringLiteral("进程监控"),
                               QStringLiteral("进程管理"), QStringLiteral("端口"),
                               QStringLiteral("mem"), QStringLiteral("memory"),
                               QStringLiteral("内存"), QStringLiteral("内存监控"),
                               QStringLiteral("内存释放"), QStringLiteral("cpu"),
                               QStringLiteral("磁盘"), QStringLiteral("杀进程"),
                               QStringLiteral("kill"), QStringLiteral("任务管理器")};
    info.description = QStringLiteral("按名字 / PID / 端口查进程，看 CPU、内存、磁盘，可结束、重启、释放内存");
    return info;
}

QWidget *ProcMonPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都新建面板：宿主不缓存面板，状态（自动释放开关、已释放累计）
    // 也就随面板一起重置，符合「离开即清空」的既有约定
    return new ProcMonPanel(parent);
}

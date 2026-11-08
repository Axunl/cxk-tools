#include "CronTool.h"

#include "CronPanel.h"

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo CronPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("cron");
    info.name = QStringLiteral("Cron 表达式");
    // 搜索关键词覆盖中英文与常见叫法：首页搜索是子串/前缀/缩写匹配，
    // crontab / 定时任务 / schedule 都是用户会想到的入口词。
    info.keywords = QStringList{
        QStringLiteral("cron"),        QStringLiteral("crontab"),
        QStringLiteral("cron表达式"),   QStringLiteral("表达式"),
        QStringLiteral("定时"),        QStringLiteral("定时任务"),
        QStringLiteral("计划任务"),     QStringLiteral("调度"),
        QStringLiteral("schedule"),    QStringLiteral("ck")};
    info.description = QStringLiteral("生成与解读 cron 表达式：中文说明、下次运行时间、常用预设");
    // iconPath 留空：宿主按展示名生成字母徽章，与其余内置插件一致
    return info;
}

QWidget *CronPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都新建面板：宿主不缓存面板，输入值不留存。
    // 内嵌面板与独立窗口面板各一份，状态互不影响。
    return new CronPanel(parent);
}

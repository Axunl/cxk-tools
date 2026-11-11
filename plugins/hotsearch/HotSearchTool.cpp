#include "HotSearchTool.h"

#include "HotSearchPanel.h"

xtools::ToolInfo HotSearchPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("hotsearch");
    info.name = QStringLiteral("今日热搜");
    info.keywords = QStringList{
        QStringLiteral("热搜"),     QStringLiteral("今日热搜"),
        QStringLiteral("热榜"),     QStringLiteral("热点"),
        QStringLiteral("微博热搜"), QStringLiteral("百度热搜"),
        QStringLiteral("知乎热榜"), QStringLiteral("抖音热榜"),
        QStringLiteral("hot"),      QStringLiteral("trending"),
    };
    info.description = QStringLiteral("查看各平台今日热搜榜单，点条目直接打开");
    return info;
}

QWidget *HotSearchPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都给新面板：宿主不缓存面板（见 docs/PLUGIN.md），
    // 热搜的价值在「今天」，每次进来重新拉一遍才是对的
    return new HotSearchPanel(parent);
}

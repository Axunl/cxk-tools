#include "FinanceTool.h"

#include "FinancePanel.h"

// ---------------------------------------------------------------------------
// 金融行情插件的入口。真正的东西都在 FinancePanel 里：
// 这里只负责元信息（首页搜索靠 keywords 命中）与「每次给一份新面板」。
//
// 关键词是插件能不能被搜到的关键：首页搜索按「名称完全相等 110 / 关键词完全相等 95 /
// 名称包含 85 / 关键词包含 65」打分，所以「黄金」「基金」「股票」这三个词必须
// 原样出现在 keywords 里（不是只写在名字或描述里），用户敲其中一个词就能把这个工具顶到第一。
// ---------------------------------------------------------------------------

xtools::ToolInfo FinancePlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("finance");
    info.name = QStringLiteral("金融行情");
    info.keywords = QStringList{
        QStringLiteral("黄金"), QStringLiteral("金价"), QStringLiteral("黄金价格"),
        QStringLiteral("基金"), QStringLiteral("净值"), QStringLiteral("基金净值"),
        QStringLiteral("股票"), QStringLiteral("股价"), QStringLiteral("股市"),
        QStringLiteral("大盘"), QStringLiteral("指数"), QStringLiteral("行情"),
        QStringLiteral("自选"), QStringLiteral("自选股"), QStringLiteral("持仓"),
        QStringLiteral("理财"), QStringLiteral("看盘"),
        QStringLiteral("gold"), QStringLiteral("fund"), QStringLiteral("stock"),
        QStringLiteral("quote"), QStringLiteral("finance"),
    };
    info.description = QStringLiteral("黄金 / 基金 / 股票行情与自选，点行复制报价");
    return info;
}

QWidget *FinancePlugin::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存面板，每次进入工具都会调用这里。
    // 自选列表存在面板构造时重新读的 ini 里，所以换面板不会丢。
    return new FinancePanel(parent);
}

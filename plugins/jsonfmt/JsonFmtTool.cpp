#include "JsonFmtTool.h"

#include "JsonFmtPanel.h"

// ---------------------------------------------------------------- 插件入口
//
// 面板实现见 JsonFmtPanel.*（语法高亮与补全在 JsonEditor.*）。
// 插件入口只负责元信息与"造一个面板"，保持这层薄，方便别人照着写新插件。

xtools::ToolInfo JsonFmtPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("jsonfmt");
    info.name = QStringLiteral("JSON 格式化");
    // 关键词决定搜索命中：英文、中文、常见动作词都放进来
    info.keywords = QStringList{QStringLiteral("json"), QStringLiteral("format"),
                                QStringLiteral("格式化"), QStringLiteral("压缩"),
                                QStringLiteral("高亮")};
    info.description = QStringLiteral("格式化 / 压缩 / 转义 JSON，支持语法高亮与键名补全");
    return info;
}

QWidget *JsonFmtPlugin::createPanel(QWidget *parent)
{
    return new JsonFmtPanel(parent);
}

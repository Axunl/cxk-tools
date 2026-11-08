#include "RegexTool.h"

#include "RegexPanel.h"

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo RegexToolPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("regex");
    info.name = QStringLiteral("正则表达式");
    // 搜索是子串 + 首字母缩写匹配：功能词（匹配 / 提取 / 测试）与俗称
    // （regex / regexp / re）都放进来，中英文都能搜到。
    info.keywords = QStringList{
        QStringLiteral("正则"), QStringLiteral("正则表达式"), QStringLiteral("正则测试"),
        QStringLiteral("匹配"), QStringLiteral("提取"), QStringLiteral("替换"),
        QStringLiteral("表达式"), QStringLiteral("模式"),
        QStringLiteral("regex"), QStringLiteral("regexp"), QStringLiteral("match"),
        QStringLiteral("pattern"),
    };
    info.description = QStringLiteral(
        "实时正则测试：高亮全部匹配，列出行号 / 位置 / 捕获组，支持 i m s x U 标志（PCRE2 语法）");
    return info;
}

QWidget *RegexToolPlugin::createPanel(QWidget *parent)
{
    return new RegexPanel(parent);
}

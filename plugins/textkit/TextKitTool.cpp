#include "TextKitTool.h"

#include "TextKitPanel.h"

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo TextKitPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("textkit");
    info.name = QStringLiteral("文本处理");
    // 搜索是子串 + 首字母缩写匹配：用户可能按功能搜（对比 / 差异 / 引号），
    // 也可能按俗称搜（diff / compare / 差集），两边都放进来。
    info.keywords = QStringList{
        QStringLiteral("文本"), QStringLiteral("文本处理"), QStringLiteral("文本工具"),
        QStringLiteral("加引号"), QStringLiteral("引号"), QStringLiteral("拼接"),
        QStringLiteral("逗号"), QStringLiteral("对比"), QStringLiteral("比较"),
        QStringLiteral("差异"), QStringLiteral("不同"), QStringLiteral("差集"),
        QStringLiteral("去重"), QStringLiteral("diff"), QStringLiteral("compare"),
        QStringLiteral("text"), QStringLiteral("quote"),
    };
    info.description = QStringLiteral("加引号拼接 '1','2','3'；两段文字差异标亮；左右两列提取各自独有的值");
    return info;
}

QWidget *TextKitPlugin::createPanel(QWidget *parent)
{
    return new TextKitPanel(parent);
}

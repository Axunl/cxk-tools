#include "ColorPickerTool.h"

#include "ColorPickerPanel.h"

// ---------------------------------------------------------------- 插件入口
// 这一层刻意保持很薄：只给元信息与「造一个新面板」，方便照着写下一个插件。
// 面板每次进入工具都会重建，颜色历史等状态都在面板实例里，不放静态变量。

xtools::ToolInfo ColorPickerPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("colorpicker");
    info.name = QStringLiteral("取色器");
    // 中英文、缩写、常见格式名都放进来：搜索是子串 + 首字母缩写匹配，
    // 用户可能搜 "取色"、"吸管"、"hex"、"rgb" 或 "cp"。
    info.keywords = QStringList{
        QStringLiteral("取色"),   QStringLiteral("取色器"), QStringLiteral("吸管"),
        QStringLiteral("颜色"),   QStringLiteral("色值"),   QStringLiteral("调色"),
        QStringLiteral("color"),  QStringLiteral("picker"), QStringLiteral("eyedropper"),
        QStringLiteral("hex"),    QStringLiteral("rgb"),    QStringLiteral("hsl"),
        QStringLiteral("cmyk"),
    };
    info.description = QStringLiteral("屏幕吸管取色，HEX / RGB / HSL / HSV / CMYK 一键复制");
    return info;
}

QWidget *ColorPickerPlugin::createPanel(QWidget *parent)
{
    return new ColorPickerPanel(parent);
}

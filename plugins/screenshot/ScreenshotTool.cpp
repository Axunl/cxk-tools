#include "ScreenshotTool.h"

#include "ScreenshotPanel.h"

// ---------------------------------------------------------------- 插件入口
// 这一层刻意保持很薄：只给元信息与「造一个新面板」。截图状态全在面板实例里，
// 不放静态变量 —— 宿主可能同时存在内嵌面板与独立窗口面板两份。

xtools::ToolInfo ScreenshotPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("screenshot");
    info.name = QStringLiteral("截图");
    // 中英文、缩写、常见叫法都放进来：搜索是子串 + 首字母缩写匹配，
    // 用户可能搜「截图」「截屏」「长截图」「screenshot」「snip」或「jt」。
    info.keywords = QStringList{
        QStringLiteral("截图"),     QStringLiteral("截屏"),      QStringLiteral("抓屏"),
        QStringLiteral("滚动截图"), QStringLiteral("长截图"),    QStringLiteral("截长图"),
        QStringLiteral("拼接"),     QStringLiteral("捕获"),      QStringLiteral("屏幕"),
        QStringLiteral("screenshot"), QStringLiteral("capture"), QStringLiteral("snip"),
        QStringLiteral("scroll"),   QStringLiteral("longshot"),  QStringLiteral("scrolling"),
    };
    info.description = QStringLiteral("框选截图与滚动长截图：拖动选区即截，滚动内容自动拼接，"
                                      "完成后自动复制到剪贴板");
    return info;
}

QWidget *ScreenshotPlugin::createPanel(QWidget *parent)
{
    return new ScreenshotPanel(parent);
}

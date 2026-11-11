#include "ScreenRecTool.h"

#include "ScreenRecPanel.h"

xtools::ToolInfo ScreenRecToolPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("screenrec");
    info.name = QStringLiteral("屏幕录制");
    // 搜索是子串 + 首字母缩写匹配，用户可能搜「录屏」「录像」「录视频」「screen」「avi」，
    // 都可能拼成录屏工具的名字，全部列上。
    info.keywords = QStringList{
        QStringLiteral("录屏"), QStringLiteral("录制"), QStringLiteral("屏幕录制"),
        QStringLiteral("录像"), QStringLiteral("录视频"), QStringLiteral("视频"),
        QStringLiteral("抓屏"), QStringLiteral("屏幕"),
        QStringLiteral("screen"), QStringLiteral("record"), QStringLiteral("recording"),
        QStringLiteral("capture"), QStringLiteral("avi"), QStringLiteral("video"),
    };
    info.description = QStringLiteral(
        "把屏幕录成视频：全屏或框选区域、可选帧率与画质、鼠标光标入画，切页不中断，录完可转 MP4");
    // iconPath 留空：宿主用字母徽章兜底，不额外往 plugins/ 拷图标文件
    return info;
}

QWidget *ScreenRecToolPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都返回全新面板。录制器是模块级单例，面板只是它的视图 ——
    // 所以即使宿主销毁了旧面板，正在进行的录制也不会断。
    return new ScreenRecPanel(parent);
}

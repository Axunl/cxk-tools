#include "DiskScanTool.h"

#include "DiskScanPanel.h"

xtools::ToolInfo DiskScanPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("diskscan");
    info.name = QStringLiteral("磁盘空间");
    // 关键词覆盖中英文、缩写和用户真正会搜的口语（占空间 / 硬盘满了 / 大文件）
    info.keywords = QStringList{
        QStringLiteral("disk"), QStringLiteral("diskscan"), QStringLiteral("space"),
        QStringLiteral("storage"), QStringLiteral("du"), QStringLiteral("windirstat"),
        QStringLiteral("磁盘"), QStringLiteral("磁盘空间"), QStringLiteral("磁盘清理"),
        QStringLiteral("空间"), QStringLiteral("硬盘"), QStringLiteral("存储"),
        QStringLiteral("占空间"), QStringLiteral("占用"), QStringLiteral("大文件"),
        QStringLiteral("文件夹大小"), QStringLiteral("清理"), QStringLiteral("缓存清理"),
        QStringLiteral("大头"), QStringLiteral("binlog"), QStringLiteral("pagefile"),
        QStringLiteral("谁占的"), QStringLiteral("哪里占空间")};
    info.description =
        QStringLiteral("统计各文件夹占多少空间，说明每项是什么、该怎么清；按「找大头」直接定位到吃空间的那一层");
    return info;
}

QWidget *DiskScanPlugin::createPanel(QWidget *parent)
{
    // 每次进入都给全新面板：宿主不缓存，扫描结果与当前位置都不留存
    return new DiskScanPanel(parent);
}

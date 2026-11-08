#include "FileLockTool.h"

#include "FileLockPanel.h"

xtools::ToolInfo FileLockPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("filelock");
    info.name = QStringLiteral("文件占用释放");
    // 覆盖中英文、常见缩写和用户真正会搜的口语（删不掉 / 被占用）
    info.keywords = QStringList{
        QStringLiteral("filelock"), QStringLiteral("lock"), QStringLiteral("unlock"),
        QStringLiteral("unlocker"), QStringLiteral("handle"), QStringLiteral("句柄"),
        QStringLiteral("文件占用"), QStringLiteral("占用"), QStringLiteral("解除占用"),
        QStringLiteral("解锁"), QStringLiteral("删不掉"), QStringLiteral("被占用"),
        QStringLiteral("强制结束"), QStringLiteral("process")};
    info.description = QStringLiteral("找出占用文件或文件夹的程序，解除占用或强制结束");
    return info;
}

QWidget *FileLockPlugin::createPanel(QWidget *parent)
{
    // 每次进入都给全新面板：宿主不缓存，路径、检测结果、勾选状态都不留存
    return new FileLockPanel(parent);
}

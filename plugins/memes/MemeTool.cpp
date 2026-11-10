#include "MemeTool.h"

#include "MemePanel.h"

xtools::ToolInfo MemePlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("memes");
    info.name = QStringLiteral("表情包");
    // 关键词决定搜索命中：中文叫法、英文叫法、常见动作词都放进来，
    // 用户搜「gif」「斗图」「meme」都该落到这里。
    info.keywords = QStringList{QStringLiteral("表情"), QStringLiteral("表情包"),
                                QStringLiteral("斗图"), QStringLiteral("gif"),
                                QStringLiteral("动图"), QStringLiteral("meme"),
                                QStringLiteral("sticker"), QStringLiteral("图片"),
                                QStringLiteral("搜索")};
    info.description = QStringLiteral("联网搜索表情包，单击复制，支持 GIF 动图");
    return info;
}

QWidget *MemePlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都会调用这里，宿主不缓存面板。网络状态、下载队列、缓存句柄
    // 全部挂在面板实例上，所以「内嵌」与「独立窗口」两份面板互不影响。
    return new meme::MemePanel(parent);
}

#include "ShareTool.h"

#include "SharePanel.h"

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo SharePlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("share");
    // 名字只叫「共享」：界面上传的是文件还是文本由面板自己分区说明，
    // 而首页磁贴按首字取徽章，「文件共享」会和「文本处理」撞成同一个「文」字。
    info.name = QStringLiteral("共享");

    // 关键词覆盖中英文与常见说法：首页搜索是子串 / 前缀 / 首字母缩写匹配，
    // 只写"文件共享"会让输 "share"、"传文件"、"局域网" 的用户搜不到。
    info.keywords = QStringList{
        QStringLiteral("文件共享"), QStringLiteral("共享"),
        // 英文旧名 fileshare 也留着：用户和文档里长期叫的是这个词，搜索应当照样命中
        QStringLiteral("share"),    QStringLiteral("fileshare"),
        QStringLiteral("局域网"),   QStringLiteral("内网"),
        QStringLiteral("传文件"),   QStringLiteral("传输"),
        QStringLiteral("文本共享"), QStringLiteral("剪贴板共享"),
        QStringLiteral("http"),     QStringLiteral("web"),
        QStringLiteral("wenjiangongxiang"), QStringLiteral("gx")};

    info.description =
        QStringLiteral("把文件与文本共享到局域网：打开即发布一个网页地址，手机/电脑用浏览器收发");
    // iconPath 留空：宿主按名字生成字母徽章，与其余内置插件一致
    return info;
}

QWidget *SharePlugin::createPanel(QWidget *parent)
{
    // 每次进入都新建面板；但共享列表与 HTTP 服务是进程级单例，
    // 所以面板换了、地址和数据都还在（见 ShareStore.h 顶部说明）。
    return new SharePanel(parent);
}

#include "QrCodeTool.h"

#include "QrCodePanel.h"

xtools::ToolInfo QrCodeToolPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("qrcode");
    info.name = QStringLiteral("二维码");
    // 搜索是子串 + 首字母缩写匹配：生成侧与识别侧的叫法都要覆盖，
    // 用户可能搜「扫码」「生成二维码」「qr」甚至「二维码识别」。
    info.keywords = QStringList{
        QStringLiteral("二维码"), QStringLiteral("扫码"), QStringLiteral("扫一扫"),
        QStringLiteral("扫描"), QStringLiteral("识别二维码"), QStringLiteral("二维码识别"),
        QStringLiteral("生成二维码"), QStringLiteral("二维码生成"),
        QStringLiteral("qr"), QStringLiteral("qrcode"), QStringLiteral("qr code"),
        QStringLiteral("barcode"), QStringLiteral("链接"), QStringLiteral("网址"),
    };
    info.description = QStringLiteral(
        "离线生成二维码（文本 / 网址 / 中文）并识别图片里的二维码，可保存 PNG 或复制图片");
    // iconPath 留空：宿主用字母徽章兜底，不额外往 plugins/ 拷图标文件
    return info;
}

QWidget *QrCodeToolPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都返回全新面板，面板内的状态（内容、图片）不跨次保留
    return new QrCodePanel(parent);
}

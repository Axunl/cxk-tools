#include "OcrTool.h"

#include "OcrPanel.h"

#include <QStringList>

xtools::ToolInfo OcrPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("ocr");
    info.name = QStringLiteral("OCR 文字识别");
    // 搜索关键词要覆盖中英文与常见叫法：用户可能输「取字」「图片转文字」「截图识别」
    info.keywords = QStringList{
        QStringLiteral("ocr"),          QStringLiteral("文字识别"),
        QStringLiteral("识别"),         QStringLiteral("截图"),
        QStringLiteral("截图识别"),     QStringLiteral("取字"),
        QStringLiteral("图片转文字"),   QStringLiteral("文字提取"),
        QStringLiteral("表格识别"),     QStringLiteral("text"),
        QStringLiteral("screenshot"),   QStringLiteral("image to text")};
    info.description = QStringLiteral("截图 / 图片 / 剪贴板提取文字，离线识别，一键复制");
    // iconPath 留空：宿主会用字母徽章兜底，不额外往 plugins/ 拷图标文件
    return info;
}

QWidget *OcrPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都返回全新面板（宿主不缓存面板，输入值不留存）；
    // 传进来的 parent 可能为 nullptr，宿主随后会 setParent 接管生命周期。
    return new OcrPanel(parent);
}

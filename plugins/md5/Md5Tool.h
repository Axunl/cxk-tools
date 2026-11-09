#pragma once

// ---------------------------------------------------------------------------
// MD5 摘要插件：文本 / 文件的 MD5 计算与比对，外加 HMAC-MD5。全部离线。
//
// 为什么叫"摘要"而不是"加密"：MD5 是单向哈希，结果不可逆，叫加密会让人误以为
// 能解回来。搜索结果里仍然收了"加密"这个关键词 —— 用户就是这么搜的。
//
// 计算规则在 Md5Logic.h（纯函数，可单独断言）；面板实现留在 .cpp 内部。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class Md5Plugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

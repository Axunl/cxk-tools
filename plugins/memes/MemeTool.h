#pragma once

// ---------------------------------------------------------------------------
// 表情包搜索插件的入口。只做两件事：报元信息、造面板。
//
// 这是本插件里唯一带 Q_OBJECT 的类 —— 面板、瓦片、网格、浮层、缓存全部用
// lambda 连接、不需要信号槽，所以 .cpp 末尾**不需要** #include "MemeTool.moc"。
//
// 元信息的关键词刻意覆盖中英文与常见叫法：搜索评分里关键词权重仅次于名称，
// 用户搜「gif」「斗图」「meme」都该能找到这里。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class MemePlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

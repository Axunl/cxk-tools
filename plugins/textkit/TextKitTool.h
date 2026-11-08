#pragma once

// ---------------------------------------------------------------------------
// 文本处理插件入口。薄薄一层：只给元信息与「造一个新面板」。
//
// 真正的功能在 TextKitPanel（三个页签）与两份纯逻辑头（TextLines.h / TextDiff.h）。
// 面板每次进入工具都会重建，所有输入内容与状态都放在面板实例里，不放静态变量。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class TextKitPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

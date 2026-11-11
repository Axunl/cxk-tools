#pragma once

// 插件入口。真正的界面与网络逻辑都在 HttpPanel 里，这里只负责元信息与建面板。

#include "ITool.h"

#include <QObject>

class HttpClientTool : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

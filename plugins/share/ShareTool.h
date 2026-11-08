#pragma once

// 文件共享插件的入口。只做两件事：报元信息、造面板。
// 真正的东西在 SharePanel（界面）、ShareServer（HTTP 服务）、ShareStore（数据）。

#include "ITool.h"

#include <QObject>

class SharePlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 三行缺一不可，少一行宿主 qobject_cast 失败
    Q_INTERFACES(xtools::ITool)

public:
    int apiVersion() const override { return XTOOLS_API_VERSION; }
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

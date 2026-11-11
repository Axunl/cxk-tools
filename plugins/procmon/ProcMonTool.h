#pragma once

// ---------------------------------------------------------------------------
// 进程监控插件的入口类。只负责元信息与创建面板，界面逻辑全在 ProcMonPanel。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class ProcMonPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 三行缺一不可
    Q_INTERFACES(xtools::ITool)

public:
    int apiVersion() const override { return XTOOLS_API_VERSION; }
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

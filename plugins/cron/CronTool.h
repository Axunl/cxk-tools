#pragma once

// ---------------------------------------------------------------------------
// Cron 表达式插件：生成、解读 cron 表达式，给出中文说明与下次运行时间。
//
// 求值与描述逻辑（CronExpr.h）是纯函数式实现，不依赖任何 UI，方便单独验证；
// 面板实现放 CronPanel.*，不暴露给宿主。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class CronPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    int apiVersion() const override { return XTOOLS_API_VERSION; }
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

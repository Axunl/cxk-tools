#pragma once

// ---------------------------------------------------------------------------
// 计算器插件：表达式求值、边输边算、回车存历史。
//
// 求值器（ExprEval）是纯函数式实现，不依赖任何 UI，方便单独验证；
// 面板实现放在 .cpp 内部，不暴露给宿主。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class CalcPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

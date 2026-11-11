#pragma once

// ---------------------------------------------------------------------------
// 金融行情插件（黄金 / 基金 / 股票 + 自选）。
// 面板实现见 FinancePanel.h，纯逻辑见 FinanceQuote.h。
// 每次 createPanel() 都返回一份全新面板，自选靠 ini 持久化，不依赖面板生命周期。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class FinancePlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 这两行不能少
    Q_INTERFACES(xtools::ITool)
public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

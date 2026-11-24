#pragma once

// ---------------------------------------------------------------------------
// 「磁盘空间」工具入口。只负责元信息与造面板，逻辑全在 DiskScanPanel。
// 与 docs/PLUGIN.md 里的最小插件完全同构：一个 QWidget 面板 + 一份元信息。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class DiskScanPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

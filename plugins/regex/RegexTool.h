#pragma once

// ---------------------------------------------------------------------------
// 正则表达式插件的入口对象。
// 宿主只认识 xtools::ITool：info() 提供元信息，createPanel() 造一个全新的面板。
// 真正的界面在 RegexPanel，匹配算法在 RegexMatch.h。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class RegexToolPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 这两行不能少，否则宿主与插件对不上
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;

    /// 每次调用都返回全新面板（宿主不缓存，见 docs/PLUGIN.md 第 4 节）。
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

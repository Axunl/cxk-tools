#pragma once

// ---------------------------------------------------------------------------
// 取色器插件入口。宿主只认这个 ITool 实现，面板与取样逻辑都是插件内部类。
//
// 分层：ColorText.h（颜色解析/格式化，纯逻辑）、ColorProbe.h（屏幕取样，纯 Qt）、
// ColorPickerPanel.*（界面）。纯逻辑单独成 header 是为了能在临时工程里直接断言，
// 不必拉起整个界面（见 docs/PLUGIN.md 第 5 节）。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class ColorPickerPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    // Qt 插件的身份证：IID 必须与 ITool.h 完全一致，缺一行 qobject_cast 就失败。
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    /// 显式对齐宿主版本，避免以后升接口时遗留一个默认返回旧版本的插件。
    int apiVersion() const override { return XTOOLS_API_VERSION; }

    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

#pragma once

// ---------------------------------------------------------------------------
// JSON 工具插件：格式化 / 压缩 / 转义 / 反转义。
//
// 面板实现在 JsonFmtPanel.* —— 只是插件内部类，宿主只认 ITool，
// 放头文件是为了让临时验证程序能直接构造面板，不必复制一份实现。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class JsonFmtPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    // 下面两行是 Qt 插件的身份证：IID 必须与 ITool.h 里的宏完全一致，
    // 缺一不可，否则 qobject_cast<ITool*> 会失败（宿主会报"不是有效的工具插件"）。
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

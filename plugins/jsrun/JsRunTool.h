#pragma once

// ---------------------------------------------------------------------------
// JS 代码运行插件：多行脚本编辑器 + 控制台输出，脚本在后台线程执行。
//
// 为什么要拆成「内核 + 面板」两半：执行侧（JsEngineRunner）是纯逻辑 + Qt 线程，
// 不依赖任何界面，可以单独搭临时工程断言「求值正确 / 死循环能被打断 / 沙箱里
// 没有宿主能力」；面板只负责取文本、显示日志与结果。
//
// 面板实现放在 .cpp 内部，不暴露给宿主（与 plugins/calc 同一写法）。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class JsRunPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    /// 显式对齐宿主版本，而不是靠接口里的默认实现：宿主升 API 时这里会立刻
    /// 编不过/加载被拒，比虚表悄悄错位好排查得多。
    int apiVersion() const override { return XTOOLS_API_VERSION; }

    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

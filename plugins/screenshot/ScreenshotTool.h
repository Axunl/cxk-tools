#pragma once

// ---------------------------------------------------------------------------
// 截图插件入口。宿主只认这个 ITool 实现，遮罩 / 拼接 / 面板都是插件内部类。
//
// 分层，便于单独验证：
//   ScreenGrab.*    抓屏与裁剪（纯 Qt，无原生调用）
//   Stitch.h        纵向拼接的纯逻辑（header-only，可跑临时断言程序）
//   CaptureOverlay.* 全屏框选遮罩（自绘，回调式，无 Q_OBJECT）
//   ScrollSession.* 抓帧循环 + 浮动控制条
//   ScreenshotPanel.* 工具主面板
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class ScreenshotPlugin : public QObject, public xtools::ITool
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

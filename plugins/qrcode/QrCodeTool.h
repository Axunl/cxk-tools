#pragma once

// ---------------------------------------------------------------------------
// 二维码插件入口。
//
// 宿主只认 xtools::ITool：这里只报元信息、造面板。
// 编码在 QrEncode，解码在 QrDecode，界面在 QrCodePanel，三层互不越界。
//
// 与 OCR 不同，本插件**不实现**可选的 IToolInput：首页搜索框粘贴图片时，
// 宿主现有的意图识别只会把图片交给 OCR；强行在二维码工具里留一个收不到调用的
// 接口方法，只会变成没人走的分支。识别入口在面板里（拖放 / 选择 / 粘贴）。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class QrCodeToolPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 三行缺一不可，少一行宿主 qobject_cast 失败
    Q_INTERFACES(xtools::ITool)

public:
    /// 显式对齐宿主接口版本，避免以后 ITool.h 升级时旧插件虚表错位
    int apiVersion() const override { return XTOOLS_API_VERSION; }

    xtools::ToolInfo info() const override;

    /// 每次调用都返回全新面板（宿主不缓存，见 docs/PLUGIN.md 第 4 节）。
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

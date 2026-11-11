#pragma once

// ---------------------------------------------------------------------------
// 屏幕录制插件入口。
//
// 宿主只认 xtools::ITool：这里只报元信息、造面板。
// 真正的录制逻辑在 RecordService（模块级单例，见其头文件），面板只是它的视图。
//
// 不实现 IToolInput：首页搜索框的意图识别只把「图片」交给 OCR、把「文本」交给
// 翻译/JSON，录屏没有"带内容进入"的语义，多实现一个收不到调用的接口只会变成
// 没人走的分支（qrcode 插件里也写了同一条理由）。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class ScreenRecToolPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 三行缺一不可，少一行宿主 qobject_cast 失败
    Q_INTERFACES(xtools::ITool)

public:
    /// 显式对齐宿主接口版本，避免以后 ITool.h 升级时旧插件虚表错位
    int apiVersion() const override { return XTOOLS_API_VERSION; }

    xtools::ToolInfo info() const override;

    /// 每次调用都返回全新面板（宿主不缓存，见 docs/PLUGIN.md 第 4 节）。
    /// 面板销毁不会影响正在进行的录制。
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

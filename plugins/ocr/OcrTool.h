#pragma once

// ---------------------------------------------------------------------------
// OCR 文字识别插件入口。
//
// 宿主只认 ITool：这里只负责报元信息与创建面板，识别/截图/排版分别在
// OcrEngine / OcrCapture / OcrText 里，互不越层。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

///
/// 插件对象。Q_OBJECT / Q_PLUGIN_METADATA / Q_INTERFACES 三行缺一不可，
/// 少一行宿主 qobject_cast<xtools::ITool*> 会失败，插件被当成加载错误。
///
class OcrPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    /// 显式对齐宿主接口版本，避免以后 ITool.h 升级时旧插件虚表错位
    int apiVersion() const override { return XTOOLS_API_VERSION; }

    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

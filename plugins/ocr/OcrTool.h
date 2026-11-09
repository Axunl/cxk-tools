#pragma once

// ---------------------------------------------------------------------------
// OCR 文字识别插件入口。
//
// 宿主只认 ITool：这里只负责报元信息与创建面板，识别/截图/排版分别在
// OcrEngine / OcrCapture / OcrText 里，互不越层。
//
// 另外实现了可选的 IToolInput：首页搜索框里粘贴图片后回车，宿主会把那张图
// 直接带进来，面板立刻识别（不用用户再点一次「剪贴板图片」）。
// ---------------------------------------------------------------------------

#include "ITool.h"
#include "IToolInput.h"

#include <QObject>
#include <QPointer>

// QPointer<T> 在类作用域里就要求 T 是完整类型（内部有 is_base_of 的 static_assert），
// 所以这里必须包含面板头文件，不能只前置声明。
#include "OcrPanel.h"

///
/// 插件对象。Q_OBJECT / Q_PLUGIN_METADATA / Q_INTERFACES 三行缺一不可，
/// 少一行宿主 qobject_cast<xtools::ITool*> 会失败，插件被当成加载错误。
///
class OcrPlugin : public QObject, public xtools::ITool, public xtools::IToolInput
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool xtools::IToolInput)

public:
    /// 显式对齐宿主接口版本，避免以后 ITool.h 升级时旧插件虚表错位
    int apiVersion() const override { return XTOOLS_API_VERSION; }

    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;

    /// 宿主约定：createPanel() 之后立刻调用。只接受图片，文本直接忽略。
    void setToolInput(const xtools::ToolInput &input) override;

private:
    /// 最近一次创建的面板（宿主每次进入工具都新建，QPointer 防悬空）
    QPointer<OcrPanel> m_panel;
};

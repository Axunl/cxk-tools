#pragma once

// ---------------------------------------------------------------------------
// JSON 工具插件：格式化 / 压缩 / 转义 / 反转义。
//
// 面板实现在 JsonFmtPanel.* —— 只是插件内部类，宿主只认 ITool，
// 放头文件是为了让临时验证程序能直接构造面板，不必复制一份实现。
//
// 另外实现了可选的 IToolInput：首页搜索识别出「用户粘了一段 JSON」时，
// 宿主会把那段文本带进来，直接落在输入框里（见 setToolInput）。
// ---------------------------------------------------------------------------

#include "ITool.h"
#include "IToolInput.h"

#include <QObject>
#include <QPointer>

// QPointer<T> 在类作用域里就要求 T 是完整类型（内部有 is_base_of 的 static_assert），
// 所以这里必须包含面板头文件，不能只前置声明。
#include "JsonFmtPanel.h"

class JsonFmtPlugin : public QObject, public xtools::ITool, public xtools::IToolInput
{
    Q_OBJECT
    // 下面两行是 Qt 插件的身份证：IID 必须与 ITool.h 里的宏完全一致，
    // 缺一不可，否则 qobject_cast<ITool*> 会失败（宿主会报"不是有效的工具插件"）。
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool xtools::IToolInput)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;

    /// 宿主约定：createPanel() 之后立刻调用。只接受文本，图片直接忽略。
    void setToolInput(const xtools::ToolInput &input) override;

private:
    /// 最近一次创建的面板。宿主每次进入工具都新建面板，且同一时刻可能同时存在
    /// 内嵌面板与独立窗口面板两份；这里只记最后一个，正好对应「刚刚创建的那一个」。
    /// QPointer 保证面板被宿主销毁后不会悬空。
    QPointer<JsonFmtPanel> m_panel;
};

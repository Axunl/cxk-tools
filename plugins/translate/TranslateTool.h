#pragma once

// ---------------------------------------------------------------------------
// 翻译插件入口：只负责向宿主报元信息与创建面板。
//
// 面板实现在 TranslatePanel.h/.cpp，服务与解析逻辑在 TranslateLogic.h ——
// 后者是纯 header（不依赖 QWidget），方便按 AGENTS.md 规则 2 直接 include 做断言测试。
// 插件只 #include "ITool.h" / "IToolInput.h"，不碰宿主内部类（分层规则）。
//
// 实现了可选的 IToolInput：首页搜不到任何工具且输入是英文时，宿主会把那段英文
// 带进来，面板直接开始翻译（用户不用再点「翻译」）。
// ---------------------------------------------------------------------------

#include "ITool.h"
#include "IToolInput.h"

#include <QObject>
#include <QPointer>

// QPointer<T> 在类作用域里就要求 T 是完整类型（内部有 is_base_of 的 static_assert），
// 所以这里必须包含面板头文件，不能只前置声明。
#include "TranslatePanel.h"

/// 宿主通过 QPluginLoader 加载本类；Q_PLUGIN_METADATA 的 IID 必须与 ITool.h 一致。
class TranslatePlugin : public QObject, public xtools::ITool, public xtools::IToolInput
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool xtools::IToolInput)

public:
    int apiVersion() const override { return XTOOLS_API_VERSION; }
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;

    /// 宿主约定：createPanel() 之后立刻调用。只接受文本，图片直接忽略。
    void setToolInput(const xtools::ToolInput &input) override;

private:
    /// 最近一次创建的面板（宿主每次进入工具都新建，QPointer 防悬空）
    QPointer<TranslatePanel> m_panel;
};

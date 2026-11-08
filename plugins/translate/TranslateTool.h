#pragma once

// ---------------------------------------------------------------------------
// 翻译插件入口：只负责向宿主报元信息与创建面板。
//
// 面板实现在 TranslatePanel.h/.cpp，服务与解析逻辑在 TranslateLogic.h ——
// 后者是纯 header（不依赖 QWidget），方便按 AGENTS.md 规则 2 直接 include 做断言测试。
// 插件只 #include "ITool.h"，不碰宿主内部类（分层规则）。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

/// 宿主通过 QPluginLoader 加载本类；Q_PLUGIN_METADATA 的 IID 必须与 ITool.h 一致。
class TranslatePlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    int apiVersion() const override { return XTOOLS_API_VERSION; }
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

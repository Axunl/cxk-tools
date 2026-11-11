#pragma once

// ---------------------------------------------------------------------------
// SQL 工具插件：MyBatis 日志还原 / SQL 格式化 / SQL 压缩。全部离线。
//
// 对应 ssformat.com 那套在线工具的三种用法，做成桌面版的好处是日志不用传到别人
// 的服务器上 —— SQL 日志里往往带着真实业务数据。
//
// 词法与排版算法在 SqlText.h（纯逻辑，可单独断言），面板实现见 SqlPanel.*，
// 这一层只负责元信息与"造一个面板"。
// ---------------------------------------------------------------------------

#include "ITool.h"
#include "IToolInput.h"

#include <QObject>
#include <QPointer>

class SqlPanel;

class SqlToolPlugin : public QObject, public xtools::ITool, public xtools::IToolInput
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool xtools::IToolInput)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
    void setToolInput(const xtools::ToolInput &input) override;

private:
    /// 最近一次创建的面板。宿主随时可能销毁它，所以用 QPointer（自动置空）。
    QPointer<SqlPanel> m_panel;
};

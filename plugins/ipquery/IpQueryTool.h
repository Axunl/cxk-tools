#pragma once

// ---------------------------------------------------------------------------
// IP 地址查询插件：列出本机各网卡 IPv4 与公网 IP（含归属地），
// 点击条目或按 Alt+数字 复制到剪贴板。
//
// 纯逻辑（网卡收集、公网响应解析）在 IpQueryLogic.h，可脱离 UI 临时测试；
// 面板实现放 .cpp 内部类，不暴露给宿主。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class IpQueryPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

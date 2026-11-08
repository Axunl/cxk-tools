#pragma once

// ---------------------------------------------------------------------------
// JWT 加解密插件：解码展示（Header/Payload/签名/时间声明）、HS* 签名校验、
// 按 Header/Payload/密钥生成令牌。全部离线。
//
// 编解码与签名逻辑在 JwtLogic.h（纯函数，可单独断言验证）；
// 面板实现放在 .cpp 内部，不暴露给宿主。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class JwtPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

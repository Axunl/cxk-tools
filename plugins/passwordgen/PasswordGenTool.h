#pragma once

// ---------------------------------------------------------------------------
// 随机密码插件：本地生成强随机密码，可选字符集、长度、易混淆字符，
// 并实时给出熵值与强度分级。全程离线，不联网、不落盘、不留历史。
//
// 生成与校验规则在 PasswordGenLogic.h（纯函数，可单独断言），
// 面板实现留在 .cpp 内部。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class PasswordGenPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

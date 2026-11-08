#pragma once

// ---------------------------------------------------------------------------
// 文件占用释放插件的入口类。只负责元信息与创建面板，平台逻辑在 FileLockProbe，
// 界面在 FileLockPanel。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class FileLockPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)   // 三行缺一不可，少一行宿主 qobject_cast 失败
    Q_INTERFACES(xtools::ITool)

public:
    int apiVersion() const override { return XTOOLS_API_VERSION; }
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

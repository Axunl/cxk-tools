#pragma once

// ---------------------------------------------------------------------------
// 「内网穿透（frpc）」插件入口。
//
// 只负责两件事：报元信息、造面板。真正的功能分别在
//   * FrpcText.h    纯逻辑：格式嗅探 / 配置校验 / 服务端地址提取 / 日志分级
//   * FrpcStore.*   配置档存档（多份配置 + 上次选中的那份）
//   * FrpcRunner.*  进程服务（模块级单例，跨页保活）与可执行文件定位
//   * FrpcPanel.*   界面
// 里，这样入口文件不会被业务代码淹掉，测试也能只挑需要的部分编进临时工程。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class FrpcToolPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;

    /// 每次进入工具都返回全新面板。当前选中的配置档、编辑内容从存档恢复
    /// （见 FrpcStore.h），所以「离开再回来」不会丢掉正在改的配置。
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

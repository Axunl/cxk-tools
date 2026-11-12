#include "FrpcTool.h"

#include "FrpcPanel.h"

xtools::ToolInfo FrpcToolPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("frpc");
    info.name = QStringLiteral("内网穿透（frpc）");
    // 关键词分三组：工具名（frpc / frp）、中文叫法（内网穿透 / 穿透 / 端口映射 / 反向代理）、
    // 以及用户嘴上常说的场景（映射端口、把本机服务暴露出去、tunnel）。
    // 只写 "frpc" 的话，想找它的人得先知道工具叫什么，等于搜不到。
    info.keywords = QStringList{QStringLiteral("frpc"), QStringLiteral("frp"),
                                QStringLiteral("内网穿透"), QStringLiteral("穿透"),
                                QStringLiteral("端口映射"), QStringLiteral("端口转发"),
                                QStringLiteral("反向代理"), QStringLiteral("映射"),
                                QStringLiteral("tunnel"), QStringLiteral("nat"),
                                QStringLiteral("代理客户端"), QStringLiteral("公网访问")};
    info.description = QStringLiteral("管理 frpc 配置、一键启动/停止内网穿透客户端，实时看日志、探服务端端口");
    return info;
}

QWidget *FrpcToolPlugin::createPanel(QWidget *parent)
{
    return new FrpcPanel(parent);
}

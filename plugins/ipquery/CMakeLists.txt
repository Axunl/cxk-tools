# IP 地址查询插件。
# 除了 xtools_api（Core/Gui/Widgets）还要链 Qt6::Network：
# QNetworkInterface 枚举网卡、QNetworkAccessManager 查公网 IP 都在 Network 模块，
# 少了会链接失败（AGENTS.md 排错表里 QLocalServer 那条同因）。
xtools_add_plugin(ipquery SOURCES IpQueryTool.cpp IpQueryTool.h IpQueryLogic.h)
target_link_libraries(ipquery PRIVATE Qt6::Network)

# 内网穿透（frpc）插件。
#
# 「探活」按钮用 QTcpSocket 连服务端端口，所以必须显式链 Qt6::Network ——
# xtools_api 只带 Core / Gui / Widgets，少了这一行会链接失败（docs/PLUGIN.md）。
# 这个模块主程序本来就在用（单实例的 QLocalServer），windeployqt 已经把
# Qt6Network.dll 部署到 bin/，插件不需要再自己拷一份。
xtools_add_plugin(frpc
    SOURCES
        FrpcTool.cpp FrpcTool.h
        FrpcPanel.cpp FrpcPanel.h
        FrpcRunner.cpp FrpcRunner.h
        FrpcStore.cpp FrpcStore.h
        FrpcText.h
)

target_link_libraries(frpc PRIVATE Qt6::Network)

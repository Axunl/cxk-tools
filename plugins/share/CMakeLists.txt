# 文件共享插件：插件自己起一个 HTTP 服务（QTcpServer / QTcpSocket），
# 所以必须额外链接 Qt Network —— xtools_api 只带 Core/Gui/Widgets，
# 少了这一行会在链接期报 QAbstractSocket 相关符号未定义（同 translate / 单实例）。
# ShareHttp.h 与 ShareWeb.h 也要列进来：前者是纯逻辑、要被临时断言程序
# include，后者是页面常量；AUTOMOC 与 IDE 的文件归属都按这份清单走。
xtools_add_plugin(share
    SOURCES
        ShareTool.cpp ShareTool.h
        SharePanel.cpp SharePanel.h
        ShareStore.cpp ShareStore.h
        ShareServer.cpp ShareServer.h
        ShareHttp.h
        ShareWeb.h
)
target_link_libraries(share PRIVATE Qt6::Network)

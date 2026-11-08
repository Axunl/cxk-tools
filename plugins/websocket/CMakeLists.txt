# WebSocket 调试插件。
# 除了 xtools_api（Core/Gui/Widgets）还要链 Qt6::Network：QTcpSocket / QSslSocket /
# QTcpServer 都在 Network 模块。**不链 Qt6::WebSockets** —— 本项目锁定的 Qt 6.8.3
# 安装里没有这个模块，客户端与服务端协议都由 WsProtocol.h 自己实现（见该文件头注释）。
xtools_add_plugin(websocket
    SOURCES
        WebSocketTool.cpp WebSocketTool.h
        WebSocketPanel.cpp WebSocketPanel.h
        WsClient.cpp WsClient.h
        WsServer.cpp WsServer.h
        WsProtocol.h
)
target_link_libraries(websocket PRIVATE Qt6::Network)

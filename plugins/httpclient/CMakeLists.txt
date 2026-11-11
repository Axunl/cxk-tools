# API 调试插件：发 HTTP 请求、看响应、存集合。
# QNetworkAccessManager / QNetworkReply 在 Qt6::Network 里，不链会链接失败
# （xtools_api 只带 Core/Gui/Widgets）。
xtools_add_plugin(httpclient
    SOURCES
        HttpClientTool.cpp HttpClientTool.h
        HttpPanel.cpp HttpPanel.h
        HttpSpec.h
        HttpStore.cpp HttpStore.h
)
target_link_libraries(httpclient PRIVATE Qt6::Network)

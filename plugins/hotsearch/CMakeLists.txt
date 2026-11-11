# 今日热搜插件。
#
# 数据全部来自网络（百度官方 board 接口 + DailyHotApi 公开实例），
# 所以除 xtools_api（只带 Core/Gui/Widgets）之外必须显式链 Qt6::Network，
# 否则 QNetworkAccessManager / QNetworkReply 会链接失败（同 ipquery / translate）。
xtools_add_plugin(hotsearch SOURCES
    HotSearchTool.cpp HotSearchTool.h
    HotSearchPanel.cpp HotSearchPanel.h
    HotSearchSource.h
)
target_link_libraries(hotsearch PRIVATE Qt6::Network)

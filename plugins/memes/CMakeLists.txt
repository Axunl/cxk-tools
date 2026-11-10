# 表情包搜索插件。
#
# 搜索页与图床都在网络上，所以除 xtools_api（只带 Core/Gui/Widgets）之外
# 还要显式链 Qt6::Network，否则 QNetworkAccessManager 会链接失败。
#
# MemeSource.h 是纯逻辑头（不依赖 QWidget/网络），临时断言工程直接 include，
# 不产生 moc 产物；本插件里唯一带 Q_OBJECT 的是 MemeTool.h。
xtools_add_plugin(memes SOURCES
    MemeTool.cpp MemeTool.h
    MemePanel.cpp MemePanel.h
    MemeGrid.cpp MemeGrid.h
    MemeTile.cpp MemeTile.h
    MemePreview.cpp MemePreview.h
    MemeCache.cpp MemeCache.h
    MemeSource.h
)
target_link_libraries(memes PRIVATE Qt6::Network)

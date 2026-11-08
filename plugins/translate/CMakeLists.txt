# 翻译插件。
# QNetworkAccessManager / QNetworkReply 属于 Qt Network 模块，xtools_api 只带
# Core/Gui/Widgets，少了这一行会链接失败（AGENTS.md 踩坑表里 QLocalServer 同因）。
# TranslateLogic.h 也要列进 SOURCES：AUTOMOC 与 IDE 的文件归属都按这份清单走。
xtools_add_plugin(translate
    SOURCES
        TranslateTool.cpp TranslateTool.h
        TranslatePanel.cpp TranslatePanel.h
        TranslateLogic.h
)
target_link_libraries(translate PRIVATE Qt6::Network)

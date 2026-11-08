# 文本处理插件。
# 只用 Qt 的 Core/Gui/Widgets（剪贴板在 Gui、编辑框在 Widgets），无第三方依赖、不联网，
# 所以这里不需要额外的 target_link_libraries（对比 plugins/ipquery 的 Qt6::Network）。
# 差异算法与行处理都在 TextLines.h / TextDiff.h 两个纯头文件里，可单独 include 做断言测试。
xtools_add_plugin(textkit
    SOURCES
        TextKitTool.cpp TextKitTool.h
        TextKitPanel.cpp TextKitPanel.h
        TextLines.h TextDiff.h
)

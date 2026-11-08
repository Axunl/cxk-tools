# 正则表达式插件。
#
# 只用 Qt 的 Core/Gui/Widgets（正则与计时在 Core、剪贴板在 Gui、编辑框在 Widgets），
# 无第三方依赖、不联网，所以这里不需要额外的 target_link_libraries。
# 匹配算法在 RegexMatch.h（纯 QtCore、无 QWidget），可单独 include 做断言测试。
xtools_add_plugin(regex
    SOURCES
        RegexTool.cpp RegexTool.h
        RegexPanel.cpp RegexPanel.h
        RegexMatch.h
)

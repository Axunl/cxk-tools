# 取色器插件。
# 不额外链库：QScreen::grabWindow / QClipboard / QCursor 都在 Qt6::Gui，
# 面板控件在 Qt6::Widgets，两者已由 xtools_api 传递进来。
# 平台差异靠 ColorProbe.h 里的说明文案处理，这里不需要任何平台专属依赖。
xtools_add_plugin(colorpicker
    SOURCES
        ColorPickerTool.cpp ColorPickerTool.h
        ColorPickerPanel.cpp ColorPickerPanel.h
        ColorText.h ColorProbe.h
)

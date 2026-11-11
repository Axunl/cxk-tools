# 截图插件。
#
# 不额外链库：QScreen::grabWindow / QClipboard / QPainter 都在 Qt6::Gui，
# 面板控件在 Qt6::Widgets，两者已由 xtools_api 传递进来。
#
# 滚动截图**不注入滚轮事件**（那样要在三端各写一套原生代码，且 macOS 需辅助功能
# 授权、Wayland 根本做不到），改为「用户自己滚、程序逐帧拼接」，因此这里没有任何
# 平台专属依赖，也不需要加条件分支。
#
# Stitch.h 是纯逻辑头文件（不参与编译，但列进来让 IDE 能索引，也让改动可见）。
xtools_add_plugin(screenshot
    SOURCES
        ScreenshotTool.cpp ScreenshotTool.h
        ScreenshotPanel.cpp ScreenshotPanel.h
        CaptureOverlay.cpp CaptureOverlay.h
        ScrollSession.cpp ScrollSession.h
        ScreenGrab.cpp ScreenGrab.h
        Stitch.h
)

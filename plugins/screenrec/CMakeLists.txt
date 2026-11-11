# 屏幕录制插件。
#
# 零第三方依赖：抓屏用 QScreen::grabWindow（Windows 上再走一条 Win32 BitBlt 快路径），
# JPEG 编码由 Qt 自带的 imageformats 插件（libjpeg-turbo）提供，AVI(MJPEG) 容器在本
# 目录内手写。因此除 xtools_api（只带 Core/Gui/Widgets）之外不需要任何第三方 link。
#
# 「转成 MP4」是**运行期**可选增强：QStandardPaths::findExecutable("ffmpeg") 找得到
# 才显示按钮，不是构建期依赖，也不进安装包。
#
# 逻辑与界面分开（照 qrcode 的组织方式）：
#   AviMjpegWriter.h  纯 AVI 封装，只用 QtCore，可被临时工程直接 include 做断言
#   FrameQueue.h      有界帧队列（QMutex + QWaitCondition）
#   ScreenGrabber.h   抓屏 + DPR 反推 + 区域裁剪 + 重入保护（QtGui，Windows 段带 GDI）
#   CursorPainter.h   光标位图获取与绘制（GDI 原生分支 + 非 Windows 的自绘降级）
#   RecordService.*   模块级录制器，跨面板保活；RecorderWorker.* 是它的写盘线程体
#   ScreenRecPanel.*  只做「取配置 → 调 service → 填控件」
#   *Overlay.*        两个浮层，都没有 Q_OBJECT，不依赖 moc
xtools_add_plugin(screenrec
    SOURCES
        ScreenRecTool.cpp ScreenRecTool.h
        ScreenRecPanel.cpp ScreenRecPanel.h
        RecordService.cpp RecordService.h
        RecorderWorker.cpp RecorderWorker.h
        RegionSelectOverlay.cpp RegionSelectOverlay.h
        CountdownOverlay.cpp CountdownOverlay.h
        AviMjpegWriter.h
        FrameQueue.h
        ScreenGrabber.h
        CursorPainter.h
)

# 抓屏快路径与光标位图用到 GDI（BitBlt / CreateDIBSection / GetDIBits）与 user32
# （GetCursorInfo / GetIconInfo）。MSVC 的默认链接库已包含这两个，这里显式写出来是
# 为了让"为什么这个插件在 Windows 上能编译"不依赖构建系统的隐含行为。
if(WIN32)
    target_link_libraries(screenrec PRIVATE user32 gdi32)
endif()

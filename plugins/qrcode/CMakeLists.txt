# 二维码工具插件。
#
# 只用 Qt 的 Core/Gui/Widgets：编码解码是纯计算（QImage 在 Gui），界面在 Widgets。
# 无第三方依赖、不联网 —— 二维码算法（编码、RS 纠错、定位与透视校正、位流解析）
# 全部在本目录内实现，所以这里不需要额外的 target_link_libraries。
#
# 逻辑与界面分开：
#   QrEncode.* / QrDecode.*  纯函数，可单独 include 做断言验证
#   QrCodePanel.*            只做取内容 → 调算法 → 填控件
xtools_add_plugin(qrcode
    SOURCES
        QrCodeTool.cpp QrCodeTool.h
        QrCodePanel.cpp QrCodePanel.h
        QrEncode.cpp QrEncode.h
        QrDecode.cpp QrDecode.h
        QrGalois.h QrLayout.h QrTables.h
)

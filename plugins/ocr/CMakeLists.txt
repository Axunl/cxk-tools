# OCR 文字识别插件。
# OcrText.h 是纯逻辑 header（不参与编译，但列进来让 IDE 能索引、也让改动可见）；
# OcrPanel.h 里带 Q_OBJECT，由 CMake 的 AUTOMOC 自动处理，无需手写 .moc include。
xtools_add_plugin(ocr
    SOURCES
        OcrTool.cpp OcrTool.h
        OcrPanel.cpp OcrPanel.h
        OcrEngine.cpp OcrEngine.h
        OcrCapture.cpp OcrCapture.h
        OcrText.h
)

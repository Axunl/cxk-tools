# OCR 文字识别插件。
# OcrText.h / OcrPostprocess.h 是纯逻辑 header（不参与编译，但列进来让 IDE 能索引、
# 也让改动可见）；OcrPanel.h 里带 Q_OBJECT，由 CMake 的 AUTOMOC 自动处理。
xtools_add_plugin(ocr
    SOURCES
        OcrTool.cpp OcrTool.h
        OcrPanel.cpp OcrPanel.h
        OcrEngine.cpp OcrEngine.h
        OcrOnnx.cpp OcrOnnx.h
        OcrCapture.cpp OcrCapture.h
        OcrText.h
        OcrPostprocess.h
)

# ---------------------------------------------------------------------------
# 可选依赖：ONNX Runtime（PP-OCR 本地识别）。
#
# 为什么做成「可选 + 找不到不报错」：onnxruntime 是 ~10MB 的第三方运行库 +
# 开发头文件，不是每个开发机/CI 都装。缺它时 ocr 插件编译成「PP-OCR 不可用、
# 回退系统 OCR / tesseract」的版本（OcrOnnx.cpp 的桩分支），整体构建不被打断。
# 发布构建（package.bat / CI）必须显式 -DONNXRUNTIME_ROOT=<解压目录> 才有完整能力。
# ---------------------------------------------------------------------------
if(XTOOLS_ENABLE_ONNX)
    find_package(Onnxruntime QUIET)
    if(Onnxruntime_FOUND)
        target_compile_definitions(ocr PRIVATE XTOOLS_HAVE_ONNX=1)
        target_link_libraries(ocr PRIVATE onnxruntime::onnxruntime)
        target_include_directories(ocr PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")

        # Windows：onnxruntime.dll 与 onnxruntime_providers_shared.dll 必须与
        # 插件产物放一起。宿主启动时从 bin/ 加载插件，插件内 onnxruntime 按 DLL
        # 搜索顺序在同目录（bin/plugins/）找不到，得放 bin/ 才能被主进程的加载器
        # 找到。放 XTOOLS_BIN_DIR 而非插件目录，与其他 Qt 运行库同层，路径最稳。
        if(WIN32 AND ONNXRUNTIME_DLL)
            add_custom_command(TARGET ocr POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_DLL}" "${XTOOLS_BIN_DIR}"
                COMMENT "Copying onnxruntime.dll to bin/"
            )
            install(FILES "${ONNXRUNTIME_DLL}" DESTINATION .)
            # providers_shared 是 onnxruntime.dll 的依赖（体积很小），漏拷会
            # 在加载 onnxruntime.dll 时报「找不到指定模块」
            if(ONNXRUNTIME_PROVIDERS_SHARED_DLL)
                add_custom_command(TARGET ocr POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${ONNXRUNTIME_PROVIDERS_SHARED_DLL}" "${XTOOLS_BIN_DIR}"
                    COMMENT "Copying onnxruntime_providers_shared.dll to bin/"
                )
                install(FILES "${ONNXRUNTIME_PROVIDERS_SHARED_DLL}" DESTINATION .)
            endif()
        endif()
    else()
        message(WARNING "XTOOLS_ENABLE_ONNX=ON 但未找到 onnxruntime（-DONNXRUNTIME_ROOT 未给或不正确）。"
                        "ocr 插件将编译为「PP-OCR 不可用、回退系统 OCR」的版本。")
    endif()
endif()

# 模型文件随程序分发，两条路：
#   1) cmake --install（发布构建）→ 落 <安装根>/plugins/ocr/models/；
#   2) POST_BUILD 拷贝（开发构建，build.bat 只 configure+build 不跑 install）→
#      落 build/bin/plugins/ocr/models/，让开发期直接运行 build/bin/cxk-tools 也能
#      用上 PP-OCR。两条路都与运行期 OcrOnnxEngine::modelDir()（可执行文件同级
#      plugins/ocr/models）对齐。为什么必须 POST_BUILD：模型目录不存在时
#      OcrOnnxEngine::isAvailable() 返回 false，OCR 会静默回退 Windows 系统 OCR，
#      而不会报错——用户只会看到「引擎：Windows 系统 OCR」却不知模型没拷。
install(DIRECTORY models/ DESTINATION plugins/ocr/models)

add_custom_command(TARGET ocr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/models" "${XTOOLS_BIN_DIR}/plugins/ocr/models"
    COMMENT "Copying PP-OCR models to bin/plugins/ocr/models/"
)

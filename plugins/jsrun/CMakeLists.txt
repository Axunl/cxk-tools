# JS 代码运行插件。
#
# QJSEngine 属于 QtQml 模块（头文件 `QtQml/qjsengine.h`），而 xtools_api 只带
# Core / Gui / Widgets，所以必须显式链 Qt6::Qml —— 根 CMakeLists 的 find_package
# 里也因此多了一个 Qml 组件。
#
# JsRunConsole.h / JsRunSamples.h 是纯数据头（不参与编译，列进来让 IDE 能索引、
# 让改动可见）。
xtools_add_plugin(jsrun
    SOURCES
        JsRunTool.cpp JsRunTool.h
        JsEngineRunner.cpp JsEngineRunner.h
        JsRunConsole.h
        JsRunStore.cpp JsRunStore.h
        JsRunSamples.h
)

target_link_libraries(jsrun PRIVATE Qt6::Qml)

if(WIN32)
    # 插件独占的 Qt 模块不会被 windeployqt 发现：它只看主程序 exe 的导入表，
    # 而 cmake/DeployQt.cmake 还按构建配置打了 stamp，不会为插件重跑。
    # 缺 Qt6Qml.dll 的表现是「插件加载失败、工具从首页静默消失」（只有 app.log
    # 里一条 warning），排查成本极高，所以构建期直接拷一份到 bin/。
    # $<TARGET_FILE:Qt6::Qml> 会按配置给出 Qt6Qml.dll / Qt6Qmld.dll。
    add_custom_command(TARGET jsrun POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::Qml>" "${XTOOLS_BIN_DIR}"
        COMMENT "Copying Qt6Qml.dll to bin/ (jsrun needs QJSEngine)")
    # 发布 staging 同样要有一份：package.bat 的 windeployqt 只处理 exe
    install(FILES "$<TARGET_FILE:Qt6::Qml>" DESTINATION .)
endif()

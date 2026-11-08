# 主程序目标。源文件是显式列表（不使用 GLOB）：新增/删除文件必须同步这里，
# 否则会出现「文件明明写了却没参与编译」的静默故障。
set(APP_SOURCES
    app/main.cpp

    core/AppController.cpp
    core/AppController.h
    core/HotkeyManager.cpp
    core/HotkeyManager.h
    core/IconBadge.cpp
    core/IconBadge.h
    core/Log.cpp
    core/Log.h
    core/LocalAppIndex.cpp
    core/LocalAppIndex.h
    core/PluginManager.cpp
    core/PluginManager.h
    core/Settings.cpp
    core/Settings.h
    core/SingleInstance.cpp
    core/SingleInstance.h
    core/ToolRegistry.cpp
    core/ToolRegistry.h
    core/WindowGeometry.h

    ui/HomePage.cpp
    ui/HomePage.h
    ui/MainWindow.cpp
    ui/MainWindow.h
    ui/SettingsPage.cpp
    ui/SettingsPage.h
    ui/ToolWindow.cpp
    ui/ToolWindow.h
    ui/TrayController.cpp
    ui/TrayController.h
)

set(APP_RESOURCES
    app/resources/resources.qrc
)

# Windows 用 WIN32 子系统，避免双击启动时弹一个黑控制台；
# macOS / Linux 保持普通可执行文件（插件目录约定为「可执行文件同级/plugins」，三平台一致）。
if(WIN32)
    qt_add_executable(cxk-tools WIN32 ${APP_SOURCES} ${APP_RESOURCES})
else()
    qt_add_executable(cxk-tools ${APP_SOURCES} ${APP_RESOURCES})
endif()

target_include_directories(cxk-tools PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
)

target_link_libraries(cxk-tools PRIVATE
    xtools_api
    Qt6::Widgets
    Qt6::Network
)

if(UNIX AND NOT APPLE)
    # Linux 全局热键：XGrabKey 走 libX11，按键事件从 Qt 的 native event filter 取（xcb 结构体）
    target_link_libraries(cxk-tools PRIVATE X11::X11)
    if(TARGET X11::xcb)
        target_link_libraries(cxk-tools PRIVATE X11::xcb)
    endif()
    if(X11_XCB_INCLUDE_PATH)
        target_include_directories(cxk-tools PRIVATE ${X11_XCB_INCLUDE_PATH})
    endif()
endif()

# 版本号只在这里注入，界面「关于」与日志都用它，避免多处写死版本字符串
target_compile_definitions(cxk-tools PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

set_target_properties(cxk-tools PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${XTOOLS_BIN_DIR}"
    OUTPUT_NAME "cxk-tools"
)

# ---------------------------------------------------------------------------
# Windows：构建后把 Qt 运行库部署到 build/bin，让「在 IDE 里点运行」和
# 「在资源管理器里双击」都能直接启动（否则会因找不到 Qt6Core.dll 而静默退出）。
# 部署脚本内部用 stamp 记账，只有首次构建才真正调用 windeployqt。
# 用 -DXTOOLS_DEPLOY_QT=OFF 可关掉（例如只想快速编译、自行管理运行库时）。
# ---------------------------------------------------------------------------
option(XTOOLS_DEPLOY_QT "构建后自动把 Qt 运行库部署到 bin/（仅 Windows）" ON)

if(WIN32 AND XTOOLS_DEPLOY_QT)
    # windeployqt 与 qmake 同目录，从 Qt 的 CMake 导入目标反查，避免再写死路径
    get_target_property(_xtools_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_xtools_qt_bin "${_xtools_qmake}" DIRECTORY)
    find_program(XTOOLS_WINDEPLOYQT windeployqt HINTS "${_xtools_qt_bin}")

    if(XTOOLS_WINDEPLOYQT)
        add_custom_command(TARGET cxk-tools POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -DXTOOLS_WINDEPLOYQT=${XTOOLS_WINDEPLOYQT}
                    -DXTOOLS_TARGET_EXE=$<TARGET_FILE:cxk-tools>
                    -DXTOOLS_BIN_DIR=${XTOOLS_BIN_DIR}
                    -DXTOOLS_CONFIG=$<CONFIG>
                    -P "${PROJECT_SOURCE_DIR}/cmake/DeployQt.cmake"
            VERBATIM
            COMMENT "确保 Qt 运行库就绪"
        )
    else()
        message(WARNING "未找到 windeployqt，IDE 内运行可能需要手动把 Qt 的 bin 加入 PATH")
    endif()
endif()

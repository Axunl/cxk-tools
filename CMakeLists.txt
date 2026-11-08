cmake_minimum_required(VERSION 3.21)

project(cxk-tools VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局编译约定
#
# C++17 是硬要求：代码里用了 std::as_const / 内联命名空间之外的若干 C++17 设施。
# CMAKE_AUTORCC 必须为 ON —— 样式表 style.qss 通过 resources.qrc 编进二进制，
# 关掉它会导致 qss 静默不生效（曾踩过：界面一片裸控件）。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTORCC ON)

# 单配置生成器（Ninja/Makefiles）没给构建类型时默认 Release；
# 多配置生成器（VS/Xcode）交给 IDE 选。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Network 是给 QLocalServer 用的（单实例），少了会链接失败。
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

if(MSVC)
    # /utf-8：source 与 execution charset 都用 UTF-8。
    # 源码里有大量中文字面量（界面文案、日志、错误提示），少了这个参数在中文
    # Windows 上会被当作 GBK 解析成乱码甚至编译报错。
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /W3)
    # NOMINMAX / WIN32_LEAN_AND_MEAN：避免 windows.h 带进 min/max 宏和一堆无用头
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

if(UNIX AND NOT APPLE)
    # Linux 全局热键走 X11（XGrabKey）。macOS 没有对应实现，故不在这里找。
    find_package(X11 REQUIRED)
endif()

# ---------------------------------------------------------------------------
# 统一输出布局（三平台一致）：
#   build/bin/cxk-tools            主程序
#   build/bin/plugins/<id>         插件动态库，宿主按「可执行文件同级/plugins」扫描
# ---------------------------------------------------------------------------
set(XTOOLS_BIN_DIR    "${CMAKE_BINARY_DIR}/bin")
set(XTOOLS_PLUGIN_DIR "${XTOOLS_BIN_DIR}/plugins")

include(cmake/XToolsHelpers.cmake)

add_subdirectory(src)

# 内置工具（插件形式）。新增工具时在这里加一行 add_subdirectory。
add_subdirectory(plugins/jsonfmt)
add_subdirectory(plugins/calc)

# ---------------------------------------------------------------------------
# 把全部插件挂到主程序的依赖链上（必须放在所有 add_subdirectory 之后）。
#
# 背景：CLion 等 IDE 的「运行」默认只构建运行配置对应的目标（cxk-tools），
# 不会构建插件这类独立 MODULE 目标 → bin/plugins 为空、启动后 0 个工具。
# 显式声明依赖后，构建 cxk-tools 必然先连带构建全部插件，IDE 里点运行即可
# 拿到完整可用的 bin/。插件清单由 xtools_add_plugin() 写入全局属性
# XTOOLS_PLUGIN_TARGETS，新增插件无需改动这里。
# ---------------------------------------------------------------------------
get_property(XTOOLS_ALL_PLUGINS GLOBAL PROPERTY XTOOLS_PLUGIN_TARGETS)
if(XTOOLS_ALL_PLUGINS)
    add_dependencies(cxk-tools ${XTOOLS_ALL_PLUGINS})
    message(STATUS "cxk-tools will also build plugins: ${XTOOLS_ALL_PLUGINS}")
endif()

cmake_minimum_required(VERSION 3.21)

# macOS 最低支持版本。README「下载与安装」与 installer/macos/Info.plist.in 的
# LSMinimumSystemVersion 都写的是 12.0，但 CMake **不会**自动把两者同步：
# 不设这个变量时，Apple clang 按「构建机所在系统」当部署目标，于是在 macos-15 /
# macos-26 runner 上产出的 .app 会变成要求 macOS 15+ / 26+，用户双击直接
# 「应用程序无法打开」。Qt 6.8 官方二进制本身的最低部署目标也是 12.0。
# 必须在 project() 之前设置；调用方用 -DCMAKE_OSX_DEPLOYMENT_TARGET=... 可覆盖。
#
# 这里用 CMAKE_HOST_APPLE 而不是 APPLE：APPLE 由 CMAKE_SYSTEM_NAME 派生，而后者在
# 第一次 project()/enable_language() 时才确定，写在 project() 前面可能还是未定义，
# 条件不成立就会静默跳过（症状依旧是「设了但没生效」，最难查）。主机平台一开始就有。
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
endif()

project(cxk-tools VERSION 0.4.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 版本号的单一来源就是上面这一行 project()。这里额外做两件事：
#
# 1) 把版本落盘成 build/xtools-version.txt。scripts/*.bat 与 CI 都读这个文件，
#    而不是去 grep CMakeLists —— 批处理解析源码格式是最容易随排版变化失效的写法，
#    而 .bat 又必须保持纯 ASCII，没法写复杂的解析逻辑。
# 2) Windows 需要 .rc 资源脚本（版本信息 + 图标）。CMake 不会因为源列表里
#    出现 .rc 就自动启用 RC 语言，必须显式 enable，否则 configure 阶段就报
#    「no RC compiler」。
# ---------------------------------------------------------------------------
file(WRITE "${CMAKE_BINARY_DIR}/xtools-version.txt" "${PROJECT_VERSION}\n")

if(WIN32)
    enable_language(RC)
endif()

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
# 可选依赖开关：OCR 的本地 PP-OCR 模型（基于 ONNX Runtime）。
#
# 默认 ON，但「找不到 onnxruntime 不报错」：缺依赖时 ocr 插件退化为「回退系统
# OCR / tesseract」的版本，不打断整体构建。要启用完整能力，配置时传：
#   -DOnnxruntime_ROOT=<onnxruntime 解压目录>
# （Find 模块也兼容旧的 -DONNXRUNTIME_ROOT 写法。）
# 发布构建（scripts/package.bat 或 CI）必须显式传该参数，否则打出来的包没有
# PP-OCR 能力（onnxruntime.dll 与模型都不会进包）。Find 模块在 cmake/FindOnnxruntime.cmake。
# ---------------------------------------------------------------------------
option(XTOOLS_ENABLE_ONNX "Build the OCR plugin with local PP-OCR (ONNX Runtime) support" ON)

# ---------------------------------------------------------------------------
# 统一输出布局（三平台一致）：
#   build/bin/cxk-tools            主程序
#   build/bin/plugins/<id>         插件动态库，宿主按「可执行文件同级/plugins」扫描
# ---------------------------------------------------------------------------
set(XTOOLS_BIN_DIR    "${CMAKE_BINARY_DIR}/bin")
set(XTOOLS_PLUGIN_DIR "${XTOOLS_BIN_DIR}/plugins")

# 让 find_package 能找到仓库自带的 Find*.cmake（目前是 FindOnnxruntime.cmake）
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(cmake/XToolsHelpers.cmake)

add_subdirectory(src)

# 内置工具（插件形式）。新增工具时在这里加一行 add_subdirectory。
add_subdirectory(plugins/jsonfmt)
add_subdirectory(plugins/calc)
add_subdirectory(plugins/ipquery)
add_subdirectory(plugins/procmon)
add_subdirectory(plugins/filelock)
add_subdirectory(plugins/ocr)
add_subdirectory(plugins/translate)
add_subdirectory(plugins/colorpicker)
add_subdirectory(plugins/jwt)
add_subdirectory(plugins/md5)
add_subdirectory(plugins/cron)
add_subdirectory(plugins/textkit)
add_subdirectory(plugins/regex)
add_subdirectory(plugins/share)
add_subdirectory(plugins/websocket)
add_subdirectory(plugins/qrcode)
add_subdirectory(plugins/memes)
add_subdirectory(plugins/finance)
add_subdirectory(plugins/hotsearch)

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

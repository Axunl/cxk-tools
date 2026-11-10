# ---------------------------------------------------------------------------
# FindOnnxruntime.cmake —— 查找 ONNX Runtime C/C++ 开发包，产出 imported target
# onnxruntime::onnxruntime。
#
# 为什么不用 find_package(onnxruntime CONFIG)：官方预编译包（onnxruntime-win-x64-*.zip /
# libonnxruntime*.tgz）不提供 CMake 的 *-config.cmake，原生 find_package 找不到。
# 所以这里手写一个 Find 模块，通过 -DONNXRUNTIME_ROOT=<解压目录> 传入根目录。
#
# 设计为「找不到也不报错」：OCR 的 ONNX 后端是可选增强，找不到时上层会编译成
# 「不可用、回退到系统 OCR / tesseract」的 stub，绝不打断整体构建（规则：依赖尽量
# 零新增，新增要能退）。调用方通过 ONNXRUNTIME_FOUND 判断是否真的可用。
#
# 查找三件套（Windows 预编译包目录结构）：
#   <root>/include/onnxruntime_cxx_api.h   头文件
#   <root>/lib/onnxruntime.lib             导入库（import lib）
#   <root>/bin/onnxruntime.dll             运行库（需拷贝到产物目录）
# macOS/Linux 的 tgz 结构是 include/ + lib/（libonnxruntime.dylib/.so），
# 本 Find 模块按名字找，平台差异交给 find_library 的 NAMES + 各平台包即可。
# ---------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)

set(_ONNXRUNTIME_HINTS)
# 兼容两种变量名：Onnxruntime_ROOT（CMake 标准 <PackageName>_ROOT，find_package
# 会自动识别）与 ONNXRUNTIME_ROOT（历史习惯）。优先标准名。
if(DEFINED Onnxruntime_ROOT)
    list(APPEND _ONNXRUNTIME_HINTS "${Onnxruntime_ROOT}")
endif()
if(DEFINED ONNXRUNTIME_ROOT)
    list(APPEND _ONNXRUNTIME_HINTS "${ONNXRUNTIME_ROOT}")
endif()

# 头文件：onnxruntime_cxx_api.h 是 C++ 入口（onnxruntime_c_api.h 是纯 C 入口）。
find_path(ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    HINTS ${_ONNXRUNTIME_HINTS}
    PATH_SUFFIXES include
)

# 库：Windows 是 onnxruntime.lib（import lib），Linux/macOS 是 libonnxruntime.so/.dylib。
# 名字统一写成 onnxruntime，find_library 会按平台补前缀/后缀。
find_library(ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    HINTS ${_ONNXRUNTIME_HINTS}
    PATH_SUFFIXES lib lib64
)

# Windows 运行库：动态链接时 onnxruntime.dll 必须与产物放一起。
# onnxruntime_providers_shared.dll 是 onnxruntime.dll 的依赖（体积很小），
# 也要一起分发，否则加载 onnxruntime.dll 时报「找不到指定模块」。
if(WIN32 AND ONNXRUNTIME_LIBRARY)
    find_file(ONNXRUNTIME_DLL
        NAMES onnxruntime.dll
        HINTS ${_ONNXRUNTIME_HINTS}
        PATH_SUFFIXES bin
    )
    find_file(ONNXRUNTIME_PROVIDERS_SHARED_DLL
        NAMES onnxruntime_providers_shared.dll
        HINTS ${_ONNXRUNTIME_HINTS}
        PATH_SUFFIXES bin
    )
endif()

find_package_handle_standard_args(Onnxruntime
    REQUIRED_VARS ONNXRUNTIME_INCLUDE_DIR ONNXRUNTIME_LIBRARY
)

if(Onnxruntime_FOUND AND NOT TARGET onnxruntime::onnxruntime)
    add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(
    ONNXRUNTIME_INCLUDE_DIR
    ONNXRUNTIME_LIBRARY
    ONNXRUNTIME_DLL
    ONNXRUNTIME_PROVIDERS_SHARED_DLL
)

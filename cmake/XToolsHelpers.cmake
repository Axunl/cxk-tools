# ---------------------------------------------------------------------------
# 构建辅助：插件接口库 + 插件目标工厂
#
# 为什么接口做成 INTERFACE 库（纯头文件）而不是编成动态库：
#   插件与宿主之间只交换 ITool 的虚表指针和 Qt 值类型，没有共享的编译产物，
#   因此不需要额外的运行库对齐问题；代价是改接口要升版本号（见 XTOOLS_API_VERSION）。
# ---------------------------------------------------------------------------
include_guard(GLOBAL)

add_library(xtools_api INTERFACE)
target_include_directories(xtools_api INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../src/api")
target_link_libraries(xtools_api INTERFACE Qt6::Core Qt6::Gui Qt6::Widgets)

# ---------------------------------------------------------------------------
# xtools_add_plugin(<target> SOURCES <files...>)
#
# 生成一个 Qt 插件（MODULE 库），输出到 build/bin/plugins/，供宿主启动时扫描。
# 这里刻意用 qt_add_library(MODULE)：MODULE 不会被链接进别的目标，符合插件语义。
# ---------------------------------------------------------------------------
function(xtools_add_plugin target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "xtools_add_plugin(${target}): missing SOURCES")
    endif()

    qt_add_library(${target} MODULE ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE xtools_api)

    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${XTOOLS_PLUGIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${XTOOLS_PLUGIN_DIR}"
        PREFIX ""              # Linux/macOS 不要 lib 前缀，文件名直接用 <id>
        FOLDER "plugins"
    )

    # MSVC 下插件与主程序必须用同一套运行库（同为 /MD），
    # 否则跨 DLL 传 QString/QWidget 会在释放时炸在不同 CRT 堆上。
    if(MSVC)
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()

    # macOS 上按惯例用 .dylib 后缀（默认 MODULE 也会给 .so，与文档/预期不一致）
    if(APPLE)
        set_target_properties(${target} PROPERTIES SUFFIX ".dylib")
    endif()

    # Ninja 不会为输出目录建父目录，插件首次构建时 plugins/ 可能还不存在
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${XTOOLS_PLUGIN_DIR}"
    )

    # 把插件目标记入全局清单（XTOOLS_PLUGIN_TARGETS），根 CMakeLists 收尾处会把
    # 这份清单整体挂到 cxk-tools 的依赖链上。为什么需要：
    #   IDE（CLion 等）的「运行」只构建运行配置选中的目标（cxk-tools），
    #   插件是彼此独立的 MODULE 目标，不在依赖链上就不会被连带构建，
    #   结果 bin/plugins 是空的、启动日志「发现 0 个候选文件」、0 个工具。
    # 用全局属性收集而不是在函数里直接 add_dependencies：与 add_subdirectory
    # 顺序无关，新增插件只要照常调用 xtools_add_plugin()，无需改别处。
    set_property(GLOBAL APPEND PROPERTY XTOOLS_PLUGIN_TARGETS ${target})
endfunction()

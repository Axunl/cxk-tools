# ---------------------------------------------------------------------------
# Windows 开发期 Qt 运行库部署（由主程序构建后的 POST_BUILD 步骤以 -P 方式调用）
#
# 为什么需要：从 IDE（CLion / Qt Creator / VS）里点「运行」时，新进程的 PATH
# 往往没有 Qt 的 bin 目录，进程会以 0xc0000135（找不到 Qt6Core.dll）直接闪退，
# 表现成「点了运行但什么也没发生」。把运行库放到 exe 旁边最省事，也让
# build/bin 下的产物可以直接双击运行。
#
# 为什么做成独立脚本而不是直接写 POST_BUILD 命令：windeployqt 一次要 1~3 秒，
# 每次增量构建都跑一遍太浪费。这里用 stamp 文件按「构建配置」记账，只有首次
# （或换了 Debug/Release、或 bin 被清空）才真正部署。
#
# 只处理主程序 exe：windeployqt 靠 exe 的导入表推依赖，看不到插件的。插件自己
# 独占的 Qt 模块由插件目标负责（例：plugins/jsrun 需要 Qt6Qml.dll，它自己的
# CMakeLists 里 POST_BUILD 拷一份到 bin/）。
#
# 入参（全部由 src/CMakeLists.txt 传入）：
#   XTOOLS_WINDEPLOYQT  windeployqt 可执行文件绝对路径
#   XTOOLS_TARGET_EXE   刚构建出来的主程序
#   XTOOLS_BIN_DIR      主程序所在目录，同时是部署目标目录
#   XTOOLS_CONFIG       构建配置名（Debug / Release / RelWithDebInfo ...）
# ---------------------------------------------------------------------------

if(NOT EXISTS "${XTOOLS_WINDEPLOYQT}")
    message(WARNING "Qt 运行库部署已跳过：找不到 windeployqt (${XTOOLS_WINDEPLOYQT})")
    return()
endif()

# Debug 构建依赖 Qt6Cored.dll，Release 依赖 Qt6Core.dll；据此判断是否已部署过
set(_core_dll "${XTOOLS_BIN_DIR}/Qt6Core.dll")
if(XTOOLS_CONFIG STREQUAL "Debug")
    set(_core_dll "${XTOOLS_BIN_DIR}/Qt6Cored.dll")
endif()

set(_stamp "${XTOOLS_BIN_DIR}/.qt-deploy-${XTOOLS_CONFIG}")
if(EXISTS "${_stamp}" AND EXISTS "${_core_dll}")
    return()
endif()

# --debug/--release 不指定的话 windeployqt 只拷 Release 版，Debug 构建照样跑不起来
if(XTOOLS_CONFIG STREQUAL "Debug")
    set(_config_flag "--debug")
else()
    set(_config_flag "--release")
endif()

# 关掉几样开发期用不上的东西（翻译、D3D 编译器、OpenGL 软件兜底、VC 运行库），
# 发布包仍走 scripts/package.bat 做完整部署。
set(_deploy_flags
    --no-translations
    --no-system-d3d-compiler
    --no-opengl-sw
    --no-compiler-runtime
)

message(STATUS "部署 Qt 运行库到 ${XTOOLS_BIN_DIR} (${XTOOLS_CONFIG})")
execute_process(
    COMMAND "${XTOOLS_WINDEPLOYQT}" ${_config_flag} ${_deploy_flags} "${XTOOLS_TARGET_EXE}"
    RESULT_VARIABLE _result
    OUTPUT_QUIET
)

if(NOT _result EQUAL 0)
    # 部署失败不影响产物本身：补上 PATH 仍可运行，所以只告警不中断构建
    message(WARNING "windeployqt 执行失败（返回 ${_result}），运行时可能需要手动把 Qt 的 bin 加进 PATH")
    return()
endif()

file(TOUCH "${_stamp}")
message(STATUS "Qt 运行库已就绪：${_core_dll}")

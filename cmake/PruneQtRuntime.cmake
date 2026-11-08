# ---------------------------------------------------------------------------
# 裁剪 staging 目录里用不到的 Qt 运行时文件。
#
# 用法（三平台一致，由 scripts/package.bat / scripts/package-macos.sh 调用）：
#   cmake -DXTools_STAGE_DIR=<staging 或 .app 路径> -P cmake/PruneQtRuntime.cmake
#
# 为什么单独做成 CMake 脚本而不是写进 .bat/.sh：
#   这份「哪些文件可以删」的清单是本项目最容易背锅的地方（删错了表现是启动
#   闪退或 HTTPS 失败），必须只有一份、并且脚本本身能算体积、能报出删了什么。
#   CMake 脚本在三个平台的 CI 上都能直接跑，不必维护两套删除逻辑。
#
# 为什么删得掉：本程序是 Qt Widgets + 光栅渲染 + 系统 TLS，
# 没有 Qt Quick / QML / OpenGL / SVG 资源，见每一项后面的说明。
# 删除任何一项前请确认对应的能力确实没被用到，否则宁可不删。
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.21)

if(NOT XTools_STAGE_DIR)
    message(FATAL_ERROR "必须用 -DXTools_STAGE_DIR=<staging> 指定待裁剪的目录")
endif()
if(NOT IS_DIRECTORY "${XTools_STAGE_DIR}")
    message(FATAL_ERROR "目录不存在: ${XTools_STAGE_DIR}")
endif()

set(_xtools_stage "${XTools_STAGE_DIR}")
set(_xtools_freed 0)
set(_xtools_removed 0)

# 计算并删除一项；无论是文件还是目录都要能处理（目录按递归求和算体积）。
function(_xtools_prune path reason)
    if(NOT EXISTS "${path}")
        return()
    endif()

    set(_bytes 0)
    if(IS_DIRECTORY "${path}")
        file(GLOB_RECURSE _inner LIST_DIRECTORIES false "${path}/*")
        foreach(_f IN LISTS _inner)
            file(SIZE "${_f}" _size)
            math(EXPR _bytes "${_bytes} + ${_size}")
        endforeach()
    else()
        file(SIZE "${path}" _bytes)
    endif()

    file(REMOVE_RECURSE "${path}")
    math(EXPR _total "${_xtools_freed} + ${_bytes}")
    set(_xtools_freed "${_total}" PARENT_SCOPE)
    math(EXPR _count "${_xtools_removed} + 1")
    set(_xtools_removed "${_count}" PARENT_SCOPE)
    message(STATUS "prune: 删除 ${path}（${_bytes} 字节）— ${reason}")
endfunction()

if(CMAKE_HOST_WIN32)
    set(_root "${_xtools_stage}")
    set(_bin "${_root}")

    # DirectX Shader Compiler：只有 Qt Quick/RHI 的 D3D 后端用，Widgets 光栅渲染不需要。
    # 这两个文件加起来约 15 MB，是本项目最大的一笔冗余。
    _xtools_prune("${_bin}/dxcompiler.dll" "DirectX Shader Compiler，Qt Widgets 用不到")
    _xtools_prune("${_bin}/dxil.dll" "DirectX Shader Compiler 依赖，同上")

    # SVG：仓库里没有 .svg，style.qss 也没有 url() 引用，图标是 IconBadge 运行期画的。
    # 若以后要支持 svg 图标，把下面三行连同 Qt6Svg.dll 一起恢复。
    _xtools_prune("${_bin}/Qt6Svg.dll" "无 SVG 图标/图片需求")
    _xtools_prune("${_bin}/imageformats/qsvg.dll" "无 SVG 解码需求")
    _xtools_prune("${_bin}/iconengines" "qsvgicon 只服务 SVG 图标")

    # TUIO 是触摸桌面的协议插件，桌面版无用。
    _xtools_prune("${_bin}/generic" "TUIO 触摸协议插件，桌面场景无用")

    # 只保留系统 TLS（SChannel）：Win10/11 上 HTTPS 走系统栈即可，
    # OpenSSL 后端与 cert-only 后端都是多余的；若将来要兼容更老系统再恢复。
    _xtools_prune("${_bin}/tls/qcertonlybackend.dll" "只做证书校验，更新检查用不到")
    _xtools_prune("${_bin}/tls/qopensslbackend.dll" "保留 SChannel 后端即可")

    # 兜底：某些 windeployqt 版本在没有 --no-opengl-sw 时会把软件 OpenGL 拷进来（约 20 MB）
    _xtools_prune("${_bin}/opengl32sw.dll" "没有 OpenGL 需求")
    _xtools_prune("${_bin}/d3dcompiler_47.dll" "没有 D3D 需求")
    # windeployqt 默认还会塞一个 25 MB 的 vc_redist.x64.exe；我们已经 app-local 部署
    # CRT DLL，不需要这个「安装时再跑一遍安装程序」的重型方案。
    _xtools_prune("${_bin}/vc_redist.x64.exe" "改由 app-local CRT DLL 满足运行库依赖")
    _xtools_prune("${_bin}/translations" "Qt 自带翻译，界面文案自备")
    _xtools_prune("${_bin}/qml" "没有 QML 运行时")
    # 随 CRT 一起拷进来的两个未使用项：C++/CX（WinRT）运行时与并发运行时库。
    _xtools_prune("${_bin}/vccorlib140.dll" "只有 C++/CX(WinRT) 会用到")
    _xtools_prune("${_bin}/concrt140.dll" "只有并发运行时(ConcRT)会用到")

elseif(CMAKE_HOST_APPLE)
    set(_contents "${_xtools_stage}/Contents")
    _xtools_prune("${_contents}/Frameworks/QtSvg.framework" "无 SVG 需求")
    _xtools_prune("${_contents}/PlugIns/imageformats/libqsvg.dylib" "无 SVG 解码需求")
    _xtools_prune("${_contents}/PlugIns/iconengines" "qsvgicon 只服务 SVG 图标")
    _xtools_prune("${_contents}/PlugIns/generic" "TUIO 触摸协议插件")
    _xtools_prune("${_contents}/Resources/translations" "Qt 自带翻译，界面文案自备")
    _xtools_prune("${_contents}/Resources/qml" "没有 QML 运行时")
    _xtools_prune("${_contents}/Frameworks/QtQml.framework" "没有 QML")
    _xtools_prune("${_contents}/Frameworks/QtQmlModels.framework" "没有 QML")
    _xtools_prune("${_contents}/Frameworks/QtQuick.framework" "没有 Qt Quick")

else()
    # Linux：Qt 运行库来自发行版，staging 里只有可执行文件和插件，没有可裁的
    # 大件；只清理可能被拷进来的调试符号与 QML 目录。
    set(_root "${_xtools_stage}")
    _xtools_prune("${_root}/qml" "没有 QML 运行时")
    _xtools_prune("${_root}/translations" "Qt 自带翻译，界面文案自备")
endif()

math(EXPR _mb "${_xtools_freed} / 1048576")
message(STATUS "prune: 共删除 ${_xtools_removed} 项，释放 ${_xtools_freed} 字节（约 ${_mb} MB）")

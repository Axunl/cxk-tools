# 文件占用释放插件。
# 平台逻辑只依赖 Qt Core（QProcess / QDir / QFileInfo），面板依赖 Widgets，
# 这两块 xtools_api 已经带上了，不需要额外模块。
xtools_add_plugin(filelock
    SOURCES
        FileLockTool.cpp FileLockTool.h
        FileLockLogic.h
        FileLockProbe.cpp FileLockProbe.h
        FileLockPanel.cpp FileLockPanel.h
)

# Windows：Restart Manager（占用检测）在 Rstrtmgr.lib 里。
# 句柄枚举用的 ntdll / kernel32 由 GetProcAddress 动态取，不需要额外库。
if(WIN32)
    target_link_libraries(filelock PRIVATE Rstrtmgr)
endif()

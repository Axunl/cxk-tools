xtools_add_plugin(procmon
    SOURCES
        ProcMonTool.cpp ProcMonTool.h
        ProcMonPanel.cpp ProcMonPanel.h
        ProcProbe.cpp ProcProbe.h
        ProcText.h
)

# Windows 的三组原生符号都在系统库里，Qt 的 CMake 不会自动带：
#   psapi   —— GetProcessMemoryInfo / EmptyWorkingSet（内存统计与回收）
#   iphlpapi—— GetExtendedTcpTable / GetExtendedUdpTable / GetIfTable2（端口与网卡流量）
#   shell32 —— CommandLineToArgvW（重启进程时切分命令行）
if(WIN32)
    target_link_libraries(procmon PRIVATE psapi iphlpapi shell32)
endif()

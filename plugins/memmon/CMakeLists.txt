xtools_add_plugin(memmon
    SOURCES
        MemMonTool.cpp MemMonTool.h
        MemMonPanel.cpp MemMonPanel.h
        MemProbe.cpp MemProbe.h
        MemText.h
)

# Windows 的 GetProcessMemoryInfo / EmptyWorkingSet 在 psapi 里（Qt 的 CMake 不会带）
if(WIN32)
    target_link_libraries(memmon PRIVATE psapi)
endif()

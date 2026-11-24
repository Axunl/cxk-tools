# 磁盘空间：纯 Qt 实现（QFileInfo / QStorageInfo / QFile::moveToTrash），
# 不需要任何原生库或额外 Qt 模块 —— 回收站也由 Qt 自己走三平台各自的实现。
xtools_add_plugin(diskscan
    SOURCES
        DiskScanTool.cpp DiskScanTool.h
        DiskScanPanel.cpp DiskScanPanel.h
        DiskScan.h
)

# Cron 表达式插件。纯 Qt Widgets，不需要额外模块。
# CronExpr.h 是纯逻辑头文件，列进 SOURCES：AUTOMOC 与 IDE 的文件归属都按这份清单走。
xtools_add_plugin(cron
    SOURCES
        CronTool.cpp CronTool.h
        CronPanel.cpp CronPanel.h
        CronExpr.h
)

# 插件入口 + 面板。
# 词法分析、格式化、压缩、MyBatis 还原都在 SqlText.h（纯 QtCore 头文件），
# 不依赖任何第三方库、不联网 —— SQL 日志里常带真实数据，不该往外发。
# SqlText.h 不含 QWidget，所以能单独 include 出来写断言程序验证。
xtools_add_plugin(sqlfmt
    SOURCES
        SqlTool.cpp SqlTool.h
        SqlPanel.cpp SqlPanel.h
        SqlText.h
)

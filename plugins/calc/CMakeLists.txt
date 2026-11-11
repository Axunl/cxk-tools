# 求值器（ExprEval.h）已移到 src/api/：宿主首页的「就地求值」与这里共用同一份实现，
# 纯头文件通过 xtools_api 的 include 路径可见，不必在这里重复列出。
xtools_add_plugin(calc SOURCES CalcTool.cpp CalcTool.h)

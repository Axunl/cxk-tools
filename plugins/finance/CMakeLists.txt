# 金融行情插件：黄金 / 基金 / 股票 + 自选。
#
# 必须显式链 Qt6::Network：行情与基金数据都是 HTTPS 请求，
# xtools_api 只带 Core/Gui/Widgets，少了这一行 QNetworkAccessManager 会链接失败
# （同 plugins/ipquery、plugins/translate）。
xtools_add_plugin(finance
    SOURCES FinanceTool.cpp FinanceTool.h FinancePanel.cpp FinancePanel.h FinanceQuote.h)
target_link_libraries(finance PRIVATE Qt6::Network)

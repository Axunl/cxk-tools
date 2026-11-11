#include "HttpClientTool.h"

#include "HttpPanel.h"

xtools::ToolInfo HttpClientTool::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("httpclient");
    info.name = QStringLiteral("API 调试");
    info.keywords = QStringList{
        QStringLiteral("api"),      QStringLiteral("http"),     QStringLiteral("https"),
        QStringLiteral("postman"),  QStringLiteral("rest"),     QStringLiteral("接口"),
        QStringLiteral("调试"),     QStringLiteral("请求"),     QStringLiteral("get"),
        QStringLiteral("post"),     QStringLiteral("put"),      QStringLiteral("delete"),
        QStringLiteral("curl"),     QStringLiteral("json"),     QStringLiteral("测试"),
    };
    info.description = QStringLiteral(
        "像 Postman 一样发 HTTP 请求：参数 / 请求头 / 请求体 / 认证、响应查看、集合与历史、cURL 导入导出");
    return info;
}

QWidget *HttpClientTool::createPanel(QWidget *parent)
{
    // 宿主不缓存面板，每次进入工具都会调用这里；HttpPanel 自己在构造时恢复草稿，
    // 所以新建面板不会丢掉上次正在编辑的请求。
    return new HttpPanel(parent);
}

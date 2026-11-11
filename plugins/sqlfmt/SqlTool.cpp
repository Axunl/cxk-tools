#include "SqlTool.h"

#include "SqlPanel.h"

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo SqlToolPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("sqlfmt");
    info.name = QStringLiteral("SQL 工具");
    // 用户可能按功能搜（格式化、压缩、美化），也可能按场景搜（MyBatis 日志、占位符），
    // 还有搜英文的（sql / format / mybatis / prettify）—— 都放进来。
    info.keywords = QStringList{
        QStringLiteral("sql"), QStringLiteral("格式化"), QStringLiteral("压缩"),
        QStringLiteral("美化"), QStringLiteral("mybatis"), QStringLiteral("日志"),
        QStringLiteral("参数"), QStringLiteral("占位符"), QStringLiteral("还原"),
        QStringLiteral("填充"), QStringLiteral("preparing"), QStringLiteral("parameters"),
        QStringLiteral("format"), QStringLiteral("minify"), QStringLiteral("prettify"),
        QStringLiteral("sqlformat"),
    };
    info.description = QStringLiteral("MyBatis 日志还原成完整 SQL；SQL 格式化与压缩，完全离线");
    return info;
}

QWidget *SqlToolPlugin::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存，输入内容不留存
    auto *panel = new SqlPanel(parent);
    m_panel = panel;   // 紧接着的 setToolInput() 要用；QPointer 会在面板销毁时自动置空
    return panel;
}

void SqlToolPlugin::setToolInput(const xtools::ToolInput &input)
{
    // 只在宿主刚创建完面板时被调用；图片输入与本工具无关，静默忽略
    if (!m_panel || input.kind != xtools::ToolInput::Kind::Text)
        return;
    m_panel->setInputText(input.text);
}

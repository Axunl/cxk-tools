#include "TranslateTool.h"

#include "TranslatePanel.h"

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo TranslatePlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("translate");
    info.name = QStringLiteral("翻译");
    // 搜索关键词覆盖中英文、拼音与常见缩写：首页搜索是子串/前缀/缩写匹配，
    // 不写全会导致用户输 "fanyi" 或 "英译中" 时找不到这个工具。
    info.keywords = QStringList{
        QStringLiteral("翻译"),      QStringLiteral("translate"),
        QStringLiteral("fanyi"),     QStringLiteral("有道"),
        QStringLiteral("youdao"),    QStringLiteral("搜狗"),
        QStringLiteral("sogou"),     QStringLiteral("中译英"),
        QStringLiteral("英译中"),    QStringLiteral("词典"),
        QStringLiteral("fy"),        QStringLiteral("translation")};
    info.description = QStringLiteral("多语言文本翻译，自动检测语言，长文本自动分段（需联网）");
    // iconPath 留空：宿主会按展示名生成字母徽章，与其余内置插件一致
    return info;
}

QWidget *TranslatePlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都新建面板：宿主不缓存面板，输入与译文都不留存。
    // 面板内部状态（在途请求、自动翻译开关、当前语言）全部是实例成员，
    // 主窗口内嵌面板与独立窗口面板各一份，互不干扰。
    auto *panel = new TranslatePanel(parent);
    m_panel = panel;   // 记住这一个，紧接着的 setToolInput() 要往里塞原文
    return panel;
}

void TranslatePlugin::setToolInput(const xtools::ToolInput &input)
{
    // 只在宿主刚创建完面板时被调用；m_panel 已失效（宿主已销毁）就什么也不做。
    // 图片输入与本工具无关，静默忽略。
    if (!m_panel || input.kind != xtools::ToolInput::Kind::Text)
        return;
    m_panel->setInputText(input.text);
}

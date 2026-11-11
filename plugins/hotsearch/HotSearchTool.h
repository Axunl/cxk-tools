#pragma once

// ---------------------------------------------------------------------------
// 今日热搜插件：查看微博 / 百度 / 知乎 / 抖音 / B 站等平台的今日热榜，
// 条目可点击直接在浏览器里打开。
//
// 纯逻辑（数据源表、接口解析、热度格式化）在 HotSearchSource.h，可脱离 UI 临时测试；
// 面板实现在 HotSearchPanel.h/.cpp，插件类只负责元信息与面板构造。
// ---------------------------------------------------------------------------

#include "ITool.h"

#include <QObject>

class HotSearchPlugin : public QObject, public xtools::ITool
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID XTOOLS_ITOOL_IID)
    Q_INTERFACES(xtools::ITool)

public:
    xtools::ToolInfo info() const override;
    QWidget *createPanel(QWidget *parent = nullptr) override;
};

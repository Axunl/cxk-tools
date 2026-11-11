#pragma once

// ---------------------------------------------------------------------------
// 金融行情面板：自选 / 黄金 / 基金 / 股票 四组报价，带名称搜索添加自选。
//
// 结构上与 plugins/hotsearch、plugins/ipquery 是同一套路：
//   - 面板自己持有 QNetworkAccessManager，请求以 this 为上下文，面板销毁即自动断开；
//   - 每次进入工具都会 createPanel() 一份新面板，所以「自选」不放在面板里，
//     而是落盘到独立的 ini（见 settingsFilePath），面板构造时重新读出来。
//
// 两个容易踩的点，写在这里省得后来人重新踩：
//   1. 切标签 / 连点刷新时会有多个请求在飞，回包顺序不保证。用 m_quoteGeneration
//      自增做代数校验，旧代数的回包直接丢掉，否则「黄金」的数据会盖到「股票」上。
//   2. 自选是唯一可增删的分组，行尾的删除按钮只在该分组出现；其余分组的行点一下
//      是「把这行报价复制到剪贴板」，不弹菜单、不跳网页。
// ---------------------------------------------------------------------------

#include "FinanceQuote.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QButtonGroup;
class QLineEdit;
class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

/// 列表区的一行（名称 + 最新价 + 涨跌幅 + 涨跌额 + 操作）。
/// 定义在 .cpp 的匿名命名空间里，这里只前置声明，减少头文件依赖。
class FinanceQuoteRow;

class FinancePanel : public QWidget
{
    Q_OBJECT

public:
    explicit FinancePanel(QWidget *parent = nullptr);
    ~FinancePanel() override;

protected:
    /// 面板可见时才开始 20 秒自动刷新：不可见时继续拉数据既浪费流量，
    /// 也会让「更新于」的时间戳变得莫名其妙。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    /// 四个分组。顺序即界面上的标签顺序。
    enum Tab
    {
        TabWatch = 0,   ///< 自选（可增删，落盘）
        TabGold,        ///< 黄金（上海金交所 / 上期所 / COMEX / NYMEX）
        TabFund,        ///< 场外基金（每日净值 + 盘中估值）
        TabStock,       ///< 股票与指数
        TabCount,
    };

    void buildUi();
    /// 列头的小字标签（固定宽度 + 右对齐，与报价行的数值列对齐）
    QLabel *makeHeaderLabel(const QString &text, int width);

    /// 切分组：清空列表区、按新分组的品种重建行、发请求
    void setTab(int tab);
    /// 当前分组要展示的品种
    QVector<finance::Symbol> symbolsForTab() const;
    /// 清空并重建列表区（含空态提示）
    void rebuildRows();
    /// 按当前分组发一批行情请求（行情一套、基金一套，各自独立成败）
    void refresh();

    /// 把一批报价写进缓存并刷新对应行；generation 对不上说明是旧回包，直接丢
    void applyQuotes(const QVector<finance::Quote> &quotes, int generation);
    /// 单个请求失败：只更新状态行，已经拿到的另一批数据照常显示
    void applyQuoteFailure(const QString &reason, int generation);
    /// 一批请求全部回来后收尾：写「更新于」或报错（错误更醒目，优先级更高）
    void finishPendingRequests();
    /// 点一行：把「名称 最新价 涨跌幅」复制到剪贴板
    void copyQuote(const QString &id);

    /// 搜索并展示候选品种（回车触发，不做逐字搜索 —— 接口没有防抖，别打那么多请求）
    void startSearch();
    void renderSearchResults();
    void clearSearch();
    /// 把一个候选品种加入自选（已存在则只提示，不重复写盘）
    void addSymbol(const finance::Symbol &symbol);
    void removeSymbol(const QString &id);

    void reloadWatchlist();
    void saveWatchlist();

    void setStatus(const QString &text, const QString &colorToken);
    void setStatusIdle();
    /// 单次请求的公共封装：超时 8 秒 + 统一 UA + 统一 Referer + 代数校验
    void sendRequest(const QUrl &url, const QString &referer,
                     int generation, bool isFund);

    QNetworkAccessManager m_nam;

    QButtonGroup *m_tabs = nullptr;
    QLineEdit *m_addEdit = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QWidget *m_searchBox = nullptr;      ///< 搜索结果区（无结果时整块隐藏）
    QVBoxLayout *m_searchLayout = nullptr;
    QWidget *m_rowsHost = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_autoRefresh = nullptr;

    int m_tab = TabWatch;

    QVector<finance::Symbol> m_watchlist;        ///< 自选，顺序即用户添加顺序
    QVector<finance::Symbol> m_currentSymbols;   ///< 当前分组正在展示的品种
    QHash<QString, ::FinanceQuoteRow *> m_rows;  ///< id → 行控件
    QHash<QString, finance::Quote> m_quotes;     ///< id → 最近一次拿到的报价（切分组时先顶上）

    int m_quoteGeneration = 0;
    int m_quotePending = 0;   ///< 还在飞的请求数，全部回来才写「更新于」
    QString m_quoteError;     ///< 本代最后一次失败原因

    int m_searchGeneration = 0;
    int m_searchPending = 0;
    QString m_searchError;      ///< 搜索失败原因（两路都失败且没有候选时用它提示，别假装「没搜到」）
    QVector<finance::Symbol> m_searchResults;
};

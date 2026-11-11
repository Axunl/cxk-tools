#pragma once

// ---------------------------------------------------------------------------
// 热搜面板：顶部选榜单 + 刷新，下面一列可点击的条目，底部是状态行与网页兜底入口。
//
// 面板自己持有一个 QNetworkAccessManager，请求跟着面板走：宿主不缓存面板，
// 离开工具页面板就被销毁，未完成的请求随之取消（回调都以 this 为上下文，
// 不会打到已释放的对象上）。
//
// 两个容易踩的点：
//   1. 切榜单 / 连点刷新时会有多个请求在飞，回包顺序不保证。用 m_generation
//      自增做代数校验，旧代数的回包直接丢掉，否则「微博」的数据会盖到「知乎」上。
//   2. 全局 qss 消掉了 QComboBox 的下拉三角（见 docs/PLUGIN.md），
//      必须自己补一枚箭头子控件，并在 eventFilter 里跟着 Resize 摆位。
// ---------------------------------------------------------------------------

#include <QNetworkAccessManager>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace hotsearch {
struct Item;
struct Result;
struct Source;
} // namespace hotsearch

class HotSearchPanel : public QWidget
{
    Q_OBJECT

public:
    explicit HotSearchPanel(QWidget *parent = nullptr);
    ~HotSearchPanel() override;

protected:
    /// 只为了把下拉箭头摆到输入框右侧 18px 区域内，其余事件全部转交基类
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();

    /// 按当前下拉选择重新拉数据（进入面板、点刷新、按 F5 都走这里）
    void loadCurrent();
    /// 发第 attempt 次请求：0 = 主接口，1 = 备用接口；没有备用就直接报错
    void requestAttempt(int sourceIndex, int attempt, int generation);

    /// attempt 只用于状态行区分「主接口」与「备用源」拿到的数据
    void applyResult(const hotsearch::Source &source, const hotsearch::Result &result, int attempt);
    void applyFailure(const hotsearch::Source &source, const QString &reason);

    /// 清空列表区（含占位行），下次填入前必须先清，否则旧榜单的条目会叠在下面
    void clearRows();
    /// 列表区的单行：排名 + 标题 + 标签 + 热度，点击打开链接
    void addRow(int rank, const hotsearch::Item &item);
    /// 列表区放一条说明（加载中 / 失败 / 空），不代表可点击的数据
    void showPlaceholder(const QString &text, bool isError);

    void setStatus(const QString &text, bool isError);
    void openInBrowser(const QString &url);

    QNetworkAccessManager m_nam;

    QComboBox *m_sourceBox = nullptr;
    QWidget *m_sourceArrow = nullptr;   ///< 自绘下拉三角（全局 qss 不画 ::drop-down）
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_siteButton = nullptr;
    QWidget *m_rowsHost = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QLabel *m_status = nullptr;

    /// 请求代数。每次 loadCurrent() 自增，回调里对不上就丢弃。
    int m_generation = 0;
};

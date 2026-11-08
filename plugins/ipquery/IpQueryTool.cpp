#include "IpQueryTool.h"

#include "IpQueryLogic.h"   // 纯收集/解析逻辑（可单独测试，见 AGENTS.md 的验证流程）

#include <QClipboard>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
// QMouseEvent 在 qwidget.h 里只是前置声明，调 event->button() 必须显式引完整定义
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScrollArea>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace {

/// 公网 IP 数据源，按序降级尝试。
/// 1) ipwho.is：HTTPS 加密，字段 ip/city/region/success/message；
/// 2) ip-api.com：免费档只有 HTTP（所以只当备胎），字段 query/city/regionName/status/message。
/// 两个来源的字段差异全部在 IpQueryLogic::parsePublicIpJson() 里抹平。
const char *const kPublicIpUrls[] = {
    "https://ipwho.is/",
    "http://ip-api.com/json/?fields=status,message,query,city,regionName,country",
};
constexpr int kPublicIpUrlCount = 2;

/// 单行条目：IP 大字 + 来源小字 + 右侧 Alt+N 提示。
/// 点击整行触发复制回调。刻意不带 Q_OBJECT（无信号槽需求），
/// 样式用 objectName 选择器（#ipRow），不依赖 moc。
class IpRow : public QFrame
{
public:
    IpRow(const QString &ip, const QString &source, const QString &shortcut,
          std::function<void()> onClick, QWidget *parent = nullptr)
        : QFrame(parent), m_onClick(std::move(onClick))
    {
        setObjectName(QStringLiteral("ipRow"));
        // QWidget 子类默认不画样式背景，不给这个属性 hover 底色不生效
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);

        auto *layout = new QHBoxLayout(this);
        // 左右 16px 留白，行高靠上下 padding 撑起（与截图的疏朗行距一致）
        layout->setContentsMargins(16, 12, 16, 12);
        layout->setSpacing(12);

        auto *textCol = new QVBoxLayout;
        textCol->setSpacing(2);

        m_ip = new QLabel(ip, this);
        m_ip->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;color:#0F172A;background:transparent;"));
        textCol->addWidget(m_ip);

        m_source = new QLabel(source, this);
        m_source->setStyleSheet(QStringLiteral("font-size:12px;color:#94A3B8;background:transparent;"));
        textCol->addWidget(m_source);

        layout->addLayout(textCol, 1);

        m_shortcut = new QLabel(shortcut, this);
        m_shortcut->setStyleSheet(QStringLiteral("font-size:12px;color:#94A3B8;background:transparent;"));
        m_shortcut->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(m_shortcut);
    }

    /// 公网行是异步填充的：先占位后改写，所以 IP/来源文本要能事后更新。
    void setTexts(const QString &ip, const QString &source)
    {
        m_ip->setText(ip);
        m_source->setText(source);
    }

protected:
    /// 鼠标落在子 QLabel 上时事件会向父控件冒泡，整行任意位置点击都算数。
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_onClick)
            m_onClick();
        QFrame::mouseReleaseEvent(event);
    }

private:
    QLabel *m_ip = nullptr;
    QLabel *m_source = nullptr;
    QLabel *m_shortcut = nullptr;
    std::function<void()> m_onClick;
};

///
/// 工具面板：本机网卡 IPv4 列表（点击/Alt+数字 复制）+ 公网 IP 行（异步查询）。
///
/// 布局与截图一致：条目行自上而下排列，本机地址在前、公网地址收尾；
/// 行右侧显示 Alt+数字 提示（超过 9 条的行只给点击复制，见 kMaxShortcutRows）。
///
class IpQueryPanel : public QWidget
{
    Q_OBJECT

public:
    explicit IpQueryPanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        // 左右 28px 与主窗口页头对齐（同 CalcPanel 的约定）
        layout->setContentsMargins(28, 0, 28, 20);
        layout->setSpacing(10);

        // 条目放滚动区：网卡多的机器（一堆虚拟网卡）不至于把行挤出可视区
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;border:none;}"));
        m_rowsHost = new QWidget(scroll);
        m_rowsHost->setStyleSheet(QStringLiteral(
            "QFrame#ipRow{border-radius:8px;background:transparent;}"
            "QFrame#ipRow:hover{background:#F1F5F9;}"));
        m_rowsLayout = new QVBoxLayout(m_rowsHost);
        // 行距 2px：行本身有 12px 内边距，再叠大的 spacing 会散架
        m_rowsLayout->setContentsMargins(0, 0, 0, 0);
        m_rowsLayout->setSpacing(2);
        m_rowsLayout->addStretch();   // 行贴顶排列，不留空档
        scroll->setWidget(m_rowsHost);
        layout->addWidget(scroll, 1);

        m_hint = new QLabel(this);
        layout->addWidget(m_hint);
        showIdleHint();

        buildRows();
        requestPublicIp(0);
    }

private:
    /// 一条可复制记录。copyable=false 的占位行（查询中/失败/无网卡）点击无效。
    struct Entry
    {
        QString ip;
        bool copyable = false;
        IpRow *row = nullptr;
    };

    /// Alt+数字 只编到第 9 条；再多的行退回点击复制（Alt+0/-/= 的惯例太冷门，不猜）。
    static constexpr int kMaxShortcutRows = 9;

    void buildRows()
    {
        // ---- 本机网卡地址：同步枚举，立即有内容可看 ----
        const auto nics = ipquery::collectLocalIpv4();
        if (nics.isEmpty()) {
            // 面板空着容易被当成坏了，给一行明确解释
            appendRow(QStringLiteral("—"),
                      QStringLiteral("未发现本机网卡 IPv4 地址"),
                      false);
        } else {
            for (const auto &nic : nics) {
                appendRow(nic.ip,
                          QStringLiteral("来自本机网卡%1").arg(nic.nicName),
                          true, /*shortcutEligible=*/true);
            }
        }

        // ---- 公网 IP 行：先占位，网络返回后就地更新 ----
        // 占位期不可复制，但它终将变可复制，所以照常编 Alt 提示（对照截图的 Alt+5）
        m_publicIndex = m_entries.size();
        appendRow(QStringLiteral("·····"),
                  QStringLiteral("正在查询公网地址…"),
                  false, /*shortcutEligible=*/true);
    }

    /// 追加一行并登记条目/快捷键。
    /// shortcutEligible 与 copyable 分开：公网行创建时不可复制（占位中），
    /// 但拿到地址后就可复制，Alt 提示要一开始就编好号；"无网卡"兜底行则永不编。
    void appendRow(const QString &ip, const QString &source, bool copyable,
                   bool shortcutEligible = false)
    {
        const int index = m_entries.size();
        QString tip;
        if (shortcutEligible && index < kMaxShortcutRows)
            tip = QStringLiteral("Alt+%1").arg(index + 1);

        auto *row = new IpRow(ip, source, tip,
                              [this, index] { copyEntry(index); },
                              m_rowsHost);
        // 新行插到 stretch 之前，保证永远贴顶排布
        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, row);

        Entry entry;
        entry.ip = ip;
        entry.copyable = copyable;
        entry.row = row;
        m_entries.append(entry);

        if (!tip.isEmpty()) {
            // WidgetWithChildrenShortcut：快捷键只在本面板里生效，
            // 不跟主窗口/独立窗口的其他 Alt 组合抢按键
            auto *shortcut = new QShortcut(
                QKeySequence(QStringLiteral("Alt+%1").arg(index + 1)), this);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut, &QShortcut::activated,
                    this, [this, index] { copyEntry(index); });
        }
    }

    void copyEntry(int index)
    {
        if (index < 0 || index >= m_entries.size())
            return;
        const Entry &entry = m_entries.at(index);
        if (!entry.copyable)
            return;   // 占位/失败行没有真地址，复制"·····"只会误导

        QGuiApplication::clipboard()->setText(entry.ip);

        // 用代数防串台：提示 2.5s 后还原，但期间用户又复制了别的地址时
        // 旧定时器不得把新提示抹掉
        const int generation = ++m_hintGeneration;
        m_hint->setText(QStringLiteral("已复制 %1 到剪贴板").arg(entry.ip));
        m_hint->setStyleSheet(QStringLiteral("font-size:12px;color:#10B981;"));
        QTimer::singleShot(2500, this, [this, generation] {
            if (generation != m_hintGeneration)
                return;
            showIdleHint();
        });
    }

    void showIdleHint()
    {
        m_hint->setText(QStringLiteral("点击条目复制 IP 到剪贴板，或按 Alt+数字"));
        m_hint->setStyleSheet(QStringLiteral("font-size:12px;color:#94A3B8;"));
    }

    // ------------------------------------------------------------ 公网 IP

    /// 请求公网地址。attempt 是数据源序号；首选失败自动降级到备胎，
    /// 全部失败时把真实原因写进公网行，不允许静默失败。
    void requestPublicIp(int attempt)
    {
        if (attempt >= kPublicIpUrlCount) {
            setPublicRowError(m_publicError);
            return;
        }

        QNetworkRequest request{QUrl(QString::fromLatin1(kPublicIpUrls[attempt]))};
        // 8s 超时：查询类小工具不该让用户对着"查询中"干等，
        // 超时走降级/报错而不是无限 pending
        request.setTransferTimeout(8000);
        // 少数接口会拒绝无 UA 的请求，统一亮明身份
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("cxk-tools/ipquery"));

        QNetworkReply *reply = m_nam.get(request);
        // 以 this 为连接上下文：面板销毁（宿主不缓存面板）时连接自动断开，
        // 不会回调到已释放的对象上
        connect(reply, &QNetworkReply::finished, this, [this, reply, attempt] {
            reply->deleteLater();

            if (reply->error() != QNetworkReply::NoError) {
                m_publicError = reply->errorString();
                requestPublicIp(attempt + 1);
                return;
            }

            QString ip, location, error;
            if (!ipquery::parsePublicIpJson(reply->readAll(), &ip, &location, &error)) {
                m_publicError = error;
                requestPublicIp(attempt + 1);
                return;
            }

            Entry &entry = m_entries[m_publicIndex];
            entry.ip = ip;
            entry.copyable = true;
            entry.row->setTexts(
                ip,
                location.isEmpty() ? QStringLiteral("公网地址")
                                   : QStringLiteral("位置： %1").arg(location));
        });
    }

    void setPublicRowError(const QString &reason)
    {
        Entry &entry = m_entries[m_publicIndex];
        entry.copyable = false;
        entry.row->setTexts(QStringLiteral("—"),
                            QStringLiteral("公网地址查询失败：%1").arg(reason));
    }

    QNetworkAccessManager m_nam;
    QList<Entry> m_entries;
    QWidget *m_rowsHost = nullptr;
    QVBoxLayout *m_rowsLayout = nullptr;
    QLabel *m_hint = nullptr;
    QString m_publicError;
    int m_publicIndex = -1;
    int m_hintGeneration = 0;
};

} // namespace

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo IpQueryPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("ipquery");
    info.name = QStringLiteral("IP 地址查询");
    info.keywords = QStringList{QStringLiteral("ip"), QStringLiteral("ip地址"),
                                QStringLiteral("地址查询"), QStringLiteral("本机ip"),
                                QStringLiteral("公网ip"), QStringLiteral("network"),
                                QStringLiteral("网卡")};
    info.description = QStringLiteral("查看本机网卡与公网 IP，点击复制");
    return info;
}

QWidget *IpQueryPlugin::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存面板，每次进入工具都会调用这里，
    // 公网地址也随每次进入重新查询（IP 可能因换网络而变）
    return new IpQueryPanel(parent);
}

// .cpp 里有带 Q_OBJECT 的类（IpQueryPanel），需要 moc 产物
#include "IpQueryTool.moc"

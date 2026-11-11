#include "HttpPanel.h"

#include "LayoutMetrics.h"
#include "ThemePalette.h"

#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

using xtools::theme::Color;

/// 列表项上的自定义数据。用角色而不是「解析 item text」：标题里可能带任意字符，
/// 靠文本反解迟早会把 URL 里的分隔符当字段分隔符。
constexpr int kMethodRole = Qt::UserRole + 1;
constexpr int kTitleRole = Qt::UserRole + 2;
constexpr int kSubtitleRole = Qt::UserRole + 3;
constexpr int kSpecRole = Qt::UserRole + 4;

/// 单次请求的空闲超时。是「多久没收到数据」而不是总时长：
/// 大文件下载只要还在持续传就不会被误杀，接口挂死则 30 秒内一定放手。
constexpr int kTransferTimeoutMs = 30000;

/// 草稿落盘防抖。每敲一个字都写盘没必要，600ms 足够覆盖「改完就切页」的情况
/// （切页时析构函数还会再同步保存一次，所以这个值只影响崩溃时的丢失范围）。
constexpr int kDraftDebounceMs = 600;

/// 下拉框右侧的小三角。全局 qss 为消掉原生 ::drop-down 在圆角外框外甩出的
/// 深色 L 形边框而接管了该子控件，副作用是 Qt 不再画箭头（详见 docs/PLUGIN.md）。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(10, 8);
        // 点击必须穿透给下拉框，否则点三角不会展开候选
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        // 跟主题走：写死灰色在深色底上几乎看不见
        painter.setBrush(isEnabled() ? xtools::theme::color(Color::TextFaint)
                                     : xtools::theme::color(Color::TextDisabled));
        QPainterPath path;
        path.moveTo(0.5, 2.0);
        path.lineTo(9.5, 2.0);
        path.lineTo(5.0, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
    }
};

/// 自带箭头、位置跟随尺寸变化的下拉框。比在面板里装 eventFilter 更好读：
/// 箭头生命周期与下拉框绑定，面板不用记得「每个 combo 都要额外养一个箭头」。
class ArrowCombo : public QComboBox
{
public:
    explicit ArrowCombo(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        m_arrow = new ComboArrow(this);
        m_arrow->show();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QComboBox::resizeEvent(event);
        placeArrow();
    }

    void showEvent(QShowEvent *event) override
    {
        QComboBox::showEvent(event);
        placeArrow();
    }

    void changeEvent(QEvent *event) override
    {
        QComboBox::changeEvent(event);
        if (event->type() == QEvent::EnabledChange && m_arrow)
            m_arrow->setEnabled(isEnabled());
    }

private:
    /// qss 给右侧留了 18px 的箭头区，10px 的三角在区里居中。
    void placeArrow()
    {
        if (!m_arrow)
            return;
        m_arrow->move(width() - 18 + 4, (height() - m_arrow->height()) / 2);
    }

    ComboArrow *m_arrow = nullptr;
};

} // namespace

/// 状态行用的单行省略标签。
///
/// QLabel 不会省略长文本，一条包含完整 URL 的错误信息会把整个工具条撑到窗口外。
/// 存的是「颜色令牌」而不是 QColor：换肤后旧色值不会自己变，重绘时必须重新取。
///
/// 刻意放在匿名命名空间之外：HttpPanel.h 里前向声明了 ElideLabel 并把它作为
/// 成员类型，放在匿名命名空间里会变成「两个不同的类型」，编译期直接报未定义。
class ElideLabel : public QLabel
{
public:
    explicit ElideLabel(QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setTextInteractionFlags(Qt::NoTextInteraction);
    }

    void setFullText(const QString &text, xtools::theme::Color token)
    {
        m_full = text;
        m_token = token;
        setToolTip(text);   // 截断的内容靠悬浮提示看全
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(xtools::theme::color(m_token));
        const QString shown = fontMetrics().elidedText(m_full, Qt::ElideRight, width() - 2);
        painter.drawText(rect().adjusted(0, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter, shown);
    }

    QSize sizeHint() const override { return QSize(120, fontMetrics().height() + 4); }
    QSize minimumSizeHint() const override { return QSize(60, fontMetrics().height() + 4); }

private:
    QString              m_full;
    xtools::theme::Color m_token = Color::TextMuted;
};

namespace {

/// 方法标签的颜色。语义色而不是品牌色：GET 绿 / POST 琥珀 / DELETE 红，
/// 扫一眼列表就能分清读接口与写接口，不用逐条读文字。
QColor methodColor(const QString &method)
{
    if (method == QLatin1String("GET"))
        return xtools::theme::color(Color::Success);
    if (method == QLatin1String("POST"))
        return xtools::theme::color(Color::Warn);
    if (method == QLatin1String("PUT"))
        return xtools::theme::color(Color::AccentText);
    if (method == QLatin1String("PATCH"))
        return xtools::theme::color(Color::SyntaxLiteral);
    if (method == QLatin1String("DELETE"))
        return xtools::theme::color(Color::Danger);
    return xtools::theme::color(Color::TextMuted);
}

/// 侧栏列表的绘制：方法徽章 + 标题 + 次要行。两行布局比 "GET https://…"
/// 一行到底更好扫，长 URL 也不会把方法名挤掉。
class RequestDelegate : public QStyledItemDelegate
{
public:
    explicit RequestDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(180, 52);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QRect row = option.rect.adjusted(2, 2, -4, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(xtools::theme::color(selected ? Color::AccentSoft : Color::BgHover));
            painter->drawRoundedRect(row, 6, 6);
        }

        const QString method = index.data(kMethodRole).toString();
        const QString title = index.data(kTitleRole).toString();
        const QString subtitle = index.data(kSubtitleRole).toString();

        // 徽章
        QFont badgeFont = option.font;
        shrinkFont(badgeFont, 2.0);
        badgeFont.setBold(true);
        painter->setFont(badgeFont);
        const QRect badge(row.left() + 8, row.top() + 9, 48, 18);
        QColor tint = methodColor(method);
        tint.setAlpha(34);
        painter->setPen(Qt::NoPen);
        painter->setBrush(tint);
        painter->drawRoundedRect(badge, 4, 4);
        painter->setPen(methodColor(method));
        const QString shownMethod =
            QFontMetrics(badgeFont).elidedText(method, Qt::ElideRight, badge.width() - 6);
        painter->drawText(badge, Qt::AlignCenter, shownMethod);

        const int textLeft = badge.right() + 8;
        const int textWidth = qMax(20, row.right() - textLeft - 8);

        // 标题
        QFont titleFont = option.font;
        titleFont.setBold(selected);
        painter->setFont(titleFont);
        painter->setPen(xtools::theme::color(selected ? Color::Text : Color::TextBody));
        const QRect titleRect(textLeft, row.top() + 6, textWidth, 18);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(titleFont).elidedText(title, Qt::ElideRight, textWidth));

        // 次要行
        QFont subFont = option.font;
        shrinkFont(subFont, 2.0);
        painter->setFont(subFont);
        painter->setPen(xtools::theme::color(Color::TextFaint));
        const QRect subRect(textLeft, row.top() + 26, textWidth, 16);
        painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(subFont).elidedText(subtitle, Qt::ElideRight, textWidth));

        painter->restore();
    }

private:
    /// 字号可能是 pointSize 也可能是 pixelSize（高 DPI 下由 fontconfig 决定），
    /// 两条路径都要缩，否则其中一种会「设置了但没变化」。
    static void shrinkFont(QFont &font, qreal points)
    {
        if (font.pointSizeF() > 0)
            font.setPointSizeF(qMax(7.0, font.pointSizeF() - points));
        else if (font.pixelSize() > 0)
            font.setPixelSize(qMax(9, font.pixelSize() - qRound(points * 1.3)));
    }
};

/// 统一的键值表格外观。表格样式在全局 qss 里没有通用规则（各插件按 objectName
/// 自己写），所以这里用 applyStyle 挂模板，换肤时宿主会重算 @token。
QTableWidget *makeTable(QWidget *parent, int columns, bool readOnly)
{
    auto *table = new QTableWidget(0, columns, parent);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(30);
    table->horizontalHeader()->setFixedHeight(30);
    table->horizontalHeader()->setHighlightSections(false);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setFrameShape(QFrame::NoFrame);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(false);
    if (readOnly)
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    else
        table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked
                               | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);

    xtools::theme::applyStyle(
        table,
        QStringLiteral("QTableWidget{background:@bg;border:1px solid @border-subtle;"
                       "border-radius:8px;gridline-color:@bg-subtle;outline:none;}"
                       "QTableWidget::item{padding:0 8px;color:@text-body;}"
                       "QTableWidget::item:selected{background:@accent-soft;color:@accent-text;}"
                       "QHeaderView::section{background:@bg;color:@text-muted;font-size:12px;"
                       "font-weight:600;border:none;border-bottom:1px solid @border;padding:0 8px;}"
                       "QTableCornerButton::section{background:@bg;border:none;}"));
    return table;
}

/// 首列是勾选框：只给 checkable + enabled，不加 editable（否则双击会进编辑态，
/// 而这一格没有文本可改）。
QTableWidgetItem *makeCheckItem(bool enabled)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    return item;
}

void appendKvRow(QTableWidget *table, const httpapi::KeyValue &kv)
{
    const int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0, makeCheckItem(kv.enabled));
    table->setItem(row, 1, new QTableWidgetItem(kv.key));
    table->setItem(row, 2, new QTableWidgetItem(kv.value));
}

/// 表格 → 键值数组。末尾的空白行（键值都为空）自动跳过，
/// 这样「留一行空行方便填下一条」不会变成发送一个空参数。
QVector<httpapi::KeyValue> collectKv(const QTableWidget *table)
{
    QVector<httpapi::KeyValue> items;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *check = table->item(row, 0);
        const QTableWidgetItem *key = table->item(row, 1);
        const QTableWidgetItem *value = table->item(row, 2);
        const QString keyText = key ? key->text().trimmed() : QString();
        const QString valueText = value ? value->text() : QString();
        if (keyText.isEmpty() && valueText.isEmpty())
            continue;

        httpapi::KeyValue kv;
        kv.enabled = check ? check->checkState() == Qt::Checked : true;
        kv.key = keyText;
        kv.value = valueText;
        items.append(kv);
    }
    return items;
}

void setKvTable(QTableWidget *table, const QVector<httpapi::KeyValue> &items)
{
    table->setRowCount(0);
    for (const httpapi::KeyValue &kv : items)
        appendKvRow(table, kv);
    // 永远留一行空行：用户想加一条参数直接点最后一格就能填，不用先找「添加」。
    // 占位行默认不勾选 —— 空行上挂一个蓝色对勾，看起来像要发一条空参数。
    appendKvRow(table, httpapi::KeyValue{false, QString(), QString()});
}

///
/// 表格编辑后的收尾：在末尾占位行里填了键名，就自动勾选它并再补一行空行。
///
/// 这是 Postman 的输入节奏：填完最后一格自动多出一行待填，不用每次都去点
/// 「添加参数」。新补的行是不勾选的，所以不会误发空参数。
///
void finishKvEdit(QTableWidget *table)
{
    const int last = table->rowCount() - 1;
    if (last < 0)
        return;

    QTableWidgetItem *check = table->item(last, 0);
    QTableWidgetItem *key = table->item(last, 1);
    const bool hasKey = key && !key->text().trimmed().isEmpty();

    if (!hasKey) {
        if (check && check->checkState() != Qt::Unchecked) {
            const QSignalBlocker blocker(table);
            check->setCheckState(Qt::Unchecked);
        }
        return;
    }

    // 插入行与改勾选都会再发信号，全部挡掉：本函数由 cellChanged 调用，
    // 不挡就会递归回到这里。
    const QSignalBlocker blocker(table);
    if (check)
        check->setCheckState(Qt::Checked);
    appendKvRow(table, httpapi::KeyValue{false, QString(), QString()});
}

int countEnabled(const QTableWidget *table)
{
    int count = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *check = table->item(row, 0);
        const QTableWidgetItem *key = table->item(row, 1);
        if (!key || key->text().trimmed().isEmpty())
            continue;
        if (check && check->checkState() != Qt::Checked)
            continue;
        ++count;
    }
    return count;
}

QString reasonPhrase(int code)
{
    switch (code) {
    case 200: return QStringLiteral("OK");
    case 201: return QStringLiteral("Created");
    case 202: return QStringLiteral("Accepted");
    case 204: return QStringLiteral("No Content");
    case 301: return QStringLiteral("Moved Permanently");
    case 302: return QStringLiteral("Found");
    case 304: return QStringLiteral("Not Modified");
    case 307: return QStringLiteral("Temporary Redirect");
    case 308: return QStringLiteral("Permanent Redirect");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 408: return QStringLiteral("Request Timeout");
    case 409: return QStringLiteral("Conflict");
    case 415: return QStringLiteral("Unsupported Media Type");
    case 422: return QStringLiteral("Unprocessable Entity");
    case 429: return QStringLiteral("Too Many Requests");
    case 500: return QStringLiteral("Internal Server Error");
    case 502: return QStringLiteral("Bad Gateway");
    case 503: return QStringLiteral("Service Unavailable");
    case 504: return QStringLiteral("Gateway Timeout");
    default: break;
    }
    return QString();
}

QString relativeTime(const QDateTime &at)
{
    if (!at.isValid())
        return QString();
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 seconds = at.secsTo(now);
    if (seconds < 0)
        return at.toString(QStringLiteral("MM-dd HH:mm"));
    if (seconds < 60)
        return QStringLiteral("刚刚");
    if (seconds < 3600)
        return QStringLiteral("%1 分钟前").arg(seconds / 60);
    if (at.date() == now.date())
        return at.toString(QStringLiteral("HH:mm"));
    return at.toString(QStringLiteral("MM-dd HH:mm"));
}

/// 集合项建议名：`GET /users/42`。比直接拿整条 URL 当名字更短，
/// 也不会因为域名太长把侧栏文字全挤掉。
QString suggestName(const httpapi::Spec &spec)
{
    const QUrl url = httpapi::toUrl(httpapi::effectiveUrl(spec));
    QString path = url.path();
    if (path.isEmpty() || path == QLatin1String("/"))
        path = url.host();
    if (path.isEmpty())
        path = spec.url;
    return spec.method.toUpper() + QLatin1Char(' ') + path;
}

httpapi::Spec specFromItem(const QListWidgetItem *item)
{
    const QByteArray raw = item->data(kSpecRole).toString().toUtf8();
    return httpapi::specFromJson(QJsonDocument::fromJson(raw).object());
}

/// 载入列表项时给用户看一眼的短描述。
QString displayName(const httpapi::Spec &spec)
{
    return spec.name.isEmpty() ? suggestName(spec) : spec.name;
}

} // namespace

// ---------------------------------------------------------------- 构造 / 析构

HttpPanel::HttpPanel(QWidget *parent)
    : QWidget(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    buildUi();

    // 恢复上次编辑的请求。草稿为空时 applySpec 也会把表格初始化成「一行空行」，
    // 不用另写一条初始化分支。
    applySpec(m_store.draft());

    refreshSavedList();
    refreshHistoryList();

    setStatus(QStringLiteral("尚未发送请求"), 0);
    setHint(QStringLiteral("Ctrl+Enter 发送 · Ctrl+S 保存到集合 · 请求只从本机发出"));
}

HttpPanel::~HttpPanel()
{
    // 面板随时可能在请求途中被宿主销毁。先断开到 this 的连接，再 abort：
    // 否则 abort 触发的 finished 会回调到正在析构的面板（m_respBody 已半死）。
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_draftTimer)
        m_draftTimer->stop();
    // 切页即销毁面板，这是把「正在编辑的请求」带回下次会话的唯一时机。
    saveDraftNow();
}

// ---------------------------------------------------------------- 搭界面

void HttpPanel::buildUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    root->setSpacing(0);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(10);
    splitter->addWidget(buildSidebar());
    splitter->addWidget(buildRequestArea());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({238, 820});
    root->addWidget(splitter);

    m_draftTimer = new QTimer(this);
    m_draftTimer->setSingleShot(true);
    m_draftTimer->setInterval(kDraftDebounceMs);
    connect(m_draftTimer, &QTimer::timeout, this, &HttpPanel::saveDraftNow);

    // Ctrl+Enter 发送：鼠标在参数表/请求体里时也能触发，不用先点回地址栏。
    auto *sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    sendShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(sendShortcut, &QShortcut::activated, this, &HttpPanel::send);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, &HttpPanel::saveCurrentToCollection);
}

QWidget *HttpPanel::buildSidebar()
{
    auto *side = new QWidget(this);
    side->setObjectName(QStringLiteral("httpSidebar"));
    side->setMinimumWidth(210);
    side->setMaximumWidth(300);
    xtools::theme::applyStyle(
        side, QStringLiteral("QWidget#httpSidebar{background:@bg-alt;border-radius:8px;}"));

    auto *layout = new QVBoxLayout(side);
    layout->setContentsMargins(10, 12, 10, 12);
    layout->setSpacing(6);

    // ---- 集合
    auto *savedRow = new QHBoxLayout;
    savedRow->setSpacing(6);
    auto *savedTitle = new QLabel(QStringLiteral("集合"), side);
    xtools::theme::applyStyle(savedTitle,
                              QStringLiteral("font-size:12px;font-weight:600;color:@text-second;"));
    auto *saveButton = new QPushButton(QStringLiteral("保存"), side);
    saveButton->setProperty("compact", true);
    saveButton->setToolTip(QStringLiteral("把当前请求存进集合（Ctrl+S）"));
    savedRow->addWidget(savedTitle);
    savedRow->addStretch(1);
    savedRow->addWidget(saveButton);
    layout->addLayout(savedRow);

    m_savedEmpty = new QLabel(QStringLiteral("还没有保存的请求。\n填好地址后点「保存」。"), side);
    m_savedEmpty->setWordWrap(true);
    xtools::theme::applyStyle(m_savedEmpty,
                              QStringLiteral("font-size:11px;color:@text-faint;padding:0 4px 4px 4px;"));
    layout->addWidget(m_savedEmpty);

    m_savedList = new QListWidget(side);
    m_savedList->setItemDelegate(new RequestDelegate(m_savedList));
    m_savedList->setFrameShape(QFrame::NoFrame);
    m_savedList->setUniformItemSizes(true);
    m_savedList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_savedList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_savedList->setMouseTracking(true);
    m_savedList->viewport()->setMouseTracking(true);
    // 选中底色由 delegate 自己画，item 自身必须透明，否则会盖住圆角。
    xtools::theme::applyStyle(
        m_savedList,
        QStringLiteral("QListWidget{background:transparent;border:none;}"
                       "QListWidget::item{background:transparent;border:none;}"
                       "QListWidget::item:selected{background:transparent;}"));
    layout->addWidget(m_savedList, 1);

    // ---- 历史
    auto *historyRow = new QHBoxLayout;
    historyRow->setSpacing(6);
    auto *historyTitle = new QLabel(QStringLiteral("历史"), side);
    xtools::theme::applyStyle(historyTitle,
                              QStringLiteral("font-size:12px;font-weight:600;color:@text-second;"));
    auto *clearButton = new QPushButton(QStringLiteral("清空"), side);
    clearButton->setProperty("compact", true);
    clearButton->setToolTip(QStringLiteral("清空全部历史（集合不受影响）"));
    historyRow->addWidget(historyTitle);
    historyRow->addStretch(1);
    historyRow->addWidget(clearButton);
    layout->addLayout(historyRow);

    m_historyEmpty = new QLabel(QStringLiteral("发送过的请求会留在这里，最多 30 条。"), side);
    m_historyEmpty->setWordWrap(true);
    xtools::theme::applyStyle(m_historyEmpty,
                              QStringLiteral("font-size:11px;color:@text-faint;padding:0 4px 4px 4px;"));
    layout->addWidget(m_historyEmpty);

    m_historyList = new QListWidget(side);
    m_historyList->setItemDelegate(new RequestDelegate(m_historyList));
    m_historyList->setFrameShape(QFrame::NoFrame);
    m_historyList->setUniformItemSizes(true);
    m_historyList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_historyList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_historyList->setMouseTracking(true);
    m_historyList->viewport()->setMouseTracking(true);
    xtools::theme::applyStyle(
        m_historyList,
        QStringLiteral("QListWidget{background:transparent;border:none;}"
                       "QListWidget::item{background:transparent;border:none;}"
                       "QListWidget::item:selected{background:transparent;}"));
    layout->addWidget(m_historyList, 1);

    connect(saveButton, &QPushButton::clicked, this, &HttpPanel::saveCurrentToCollection);
    connect(clearButton, &QPushButton::clicked, this, &HttpPanel::clearHistory);
    connect(m_savedList, &QListWidget::itemClicked, this, &HttpPanel::loadListItem);
    connect(m_historyList, &QListWidget::itemClicked, this, &HttpPanel::loadListItem);
    connect(m_savedList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showListMenu(pos, true); });
    connect(m_historyList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) { showListMenu(pos, false); });

    // Delete 删集合项：侧栏列表常驻，键盘操作比右键更快。
    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, m_savedList);
    deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteShortcut, &QShortcut::activated, this,
            [this] { deleteSavedItem(m_savedList->currentRow()); });

    return side;
}

QWidget *HttpPanel::buildRequestArea()
{
    auto *area = new QWidget(this);
    auto *layout = new QVBoxLayout(area);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // ---- 方法 + 地址 + 发送
    auto *urlRow = new QHBoxLayout;
    urlRow->setSpacing(8);

    m_method = new ArrowCombo(area);
    m_method->addItems(httpapi::methods());
    m_method->setFixedWidth(104);
    m_method->setFixedHeight(34);
    m_method->setToolTip(QStringLiteral("请求方法；从 cURL 导入自定义动词时会临时加进列表"));

    m_url = new QLineEdit(area);
    m_url->setPlaceholderText(
        QStringLiteral("接口地址，如 https://httpbin.org/get 或 localhost:8080/api/users"));
    m_url->setClearButtonEnabled(true);
    m_url->setMinimumHeight(34);

    m_send = new QPushButton(QStringLiteral("发送"), area);
    m_send->setProperty("accent", true);
    m_send->setMinimumWidth(84);
    m_send->setFixedHeight(34);
    m_send->setToolTip(QStringLiteral("发送请求（Ctrl+Enter）；请求进行中再点一次是取消"));

    urlRow->addWidget(m_method);
    urlRow->addWidget(m_url, 1);
    urlRow->addWidget(m_send);
    layout->addLayout(urlRow);

    // ---- 请求配置页签
    m_reqTabs = new QTabWidget(area);
    m_reqTabs->setDocumentMode(true);
    m_reqTabs->addTab(buildParamsTab(), QStringLiteral("参数"));
    m_reqTabs->addTab(buildHeadersTab(), QStringLiteral("请求头"));
    m_reqTabs->addTab(buildBodyTab(), QStringLiteral("请求体"));
    m_reqTabs->addTab(buildAuthTab(), QStringLiteral("认证"));

    auto *corner = new QWidget(m_reqTabs);
    auto *cornerLayout = new QHBoxLayout(corner);
    cornerLayout->setContentsMargins(0, 0, 4, 0);
    cornerLayout->setSpacing(6);
    auto *importButton = new QPushButton(QStringLiteral("导入 cURL"), corner);
    importButton->setProperty("compact", true);
    // 页签栏高度只有约 26px，而全局按钮是 padding 6px + min-height 20px（约 34px 高）。
    // 放进 cornerWidget 会被页签栏裁掉上下各一截，文字只剩半行 —— 压到 24px 并缩小
    // 内边距才能完整显示。两个按钮都要处理，别只改一个。
    importButton->setFixedHeight(24);
    xtools::theme::applyStyle(importButton,
                              QStringLiteral("QPushButton{padding:2px 10px;font-size:12px;}"));
    importButton->setToolTip(QStringLiteral("把剪贴板里的 curl 命令还原成这个请求"));
    auto *curlButton = new QPushButton(QStringLiteral("复制 cURL"), corner);
    curlButton->setProperty("compact", true);
    curlButton->setFixedHeight(24);
    xtools::theme::applyStyle(curlButton,
                              QStringLiteral("QPushButton{padding:2px 10px;font-size:12px;}"));
    curlButton->setToolTip(QStringLiteral("复制成可直接在终端执行的 curl 命令（含认证头）"));
    cornerLayout->addWidget(importButton);
    cornerLayout->addWidget(curlButton);
    m_reqTabs->setCornerWidget(corner, Qt::TopRightCorner);

    connect(importButton, &QPushButton::clicked, this, &HttpPanel::importCurl);
    connect(curlButton, &QPushButton::clicked, this, &HttpPanel::copyCurl);

    // ---- 上下分栏：请求配置 / 响应。用户常把响应拉大看长 JSON，必须可拖。
    auto *vertical = new QSplitter(Qt::Vertical, area);
    vertical->setChildrenCollapsible(false);
    vertical->setHandleWidth(10);
    vertical->addWidget(m_reqTabs);
    vertical->addWidget(buildResponseArea());
    vertical->setStretchFactor(0, 0);
    vertical->setStretchFactor(1, 1);
    vertical->setSizes({250, 330});
    layout->addWidget(vertical, 1);

    m_hint = new QLabel(area);
    m_hint->setWordWrap(true);
    xtools::theme::applyStyle(m_hint, QStringLiteral("font-size:12px;color:@text-faint;"));
    layout->addWidget(m_hint);

    // ---- 信号
    connect(m_send, &QPushButton::clicked, this, &HttpPanel::send);
    connect(m_url, &QLineEdit::returnPressed, this, &HttpPanel::send);
    connect(m_url, &QLineEdit::editingFinished, this, &HttpPanel::refreshParamsFromUrl);
    connect(m_url, &QLineEdit::textChanged, this, &HttpPanel::scheduleDraftSave);
    connect(m_method, &QComboBox::currentTextChanged, this, [this] {
        updateTabLabels();
        scheduleDraftSave();
    });

    return area;
}

QWidget *HttpPanel::buildParamsTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    m_params = makeTable(page, 3, false);
    m_params->setHorizontalHeaderLabels(
        {QStringLiteral("启用"), QStringLiteral("参数名"), QStringLiteral("参数值")});
    m_params->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_params->setColumnWidth(0, 40);
    m_params->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_params->setColumnWidth(1, 220);
    m_params->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_params, 1);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *add = new QPushButton(QStringLiteral("添加参数"), page);
    add->setProperty("compact", true);
    auto *remove = new QPushButton(QStringLiteral("删除选中"), page);
    remove->setProperty("compact", true);
    auto *hint = new QLabel(QStringLiteral("勾选才会发送；地址栏里的查询串会自动同步到这张表"), page);
    xtools::theme::applyStyle(hint, QStringLiteral("font-size:11px;color:@text-faint;"));
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    row->addWidget(hint);
    layout->addLayout(row);

    connect(add, &QPushButton::clicked, this, [this] {
        appendKvRow(m_params, httpapi::KeyValue{true, QString(), QString()});
        const int rowIndex = m_params->rowCount() - 1;
        m_params->setCurrentCell(rowIndex, 1);
        m_params->editItem(m_params->item(rowIndex, 1));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int index = m_params->currentRow();
        if (index < 0 || m_params->rowCount() <= 1)
            return;
        m_params->removeRow(index);
        rewriteUrlFromParams();
    });
    connect(m_params, &QTableWidget::cellChanged, this, [this] {
        // 程序化填表（载入草稿/集合、地址栏同步）也会发这个信号。不挡住的话
        // finishKvEdit 会把「禁用」的勾重新打上，还会多补几行空行。
        if (m_syncing)
            return;
        finishKvEdit(m_params);
        updateTabLabels();
        rewriteUrlFromParams();
        scheduleDraftSave();
    });
    connect(m_params, &QTableWidget::itemChanged, this, [this] {
        if (m_syncing)
            return;
        updateTabLabels();
        rewriteUrlFromParams();
        scheduleDraftSave();
    });

    return page;
}

QWidget *HttpPanel::buildHeadersTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    m_headers = makeTable(page, 3, false);
    m_headers->setHorizontalHeaderLabels(
        {QStringLiteral("启用"), QStringLiteral("请求头"), QStringLiteral("值")});
    m_headers->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_headers->setColumnWidth(0, 40);
    m_headers->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_headers->setColumnWidth(1, 220);
    m_headers->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_headers, 1);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *add = new QPushButton(QStringLiteral("添加请求头"), page);
    add->setProperty("compact", true);
    auto *remove = new QPushButton(QStringLiteral("删除选中"), page);
    remove->setProperty("compact", true);
    auto *hint = new QLabel(QStringLiteral("Content-Type 不填会按请求体类型自动补；认证页也会自动生成 Authorization"),
                            page);
    xtools::theme::applyStyle(hint, QStringLiteral("font-size:11px;color:@text-faint;"));
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    row->addWidget(hint);
    layout->addLayout(row);

    connect(add, &QPushButton::clicked, this, [this] {
        appendKvRow(m_headers, httpapi::KeyValue{true, QString(), QString()});
        const int rowIndex = m_headers->rowCount() - 1;
        m_headers->setCurrentCell(rowIndex, 1);
        m_headers->editItem(m_headers->item(rowIndex, 1));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int index = m_headers->currentRow();
        if (index < 0 || m_headers->rowCount() <= 1)
            return;
        m_headers->removeRow(index);
        updateTabLabels();
        scheduleDraftSave();
    });
    connect(m_headers, &QTableWidget::cellChanged, this, [this] {
        if (m_syncing)
            return;
        finishKvEdit(m_headers);
        updateTabLabels();
        scheduleDraftSave();
    });
    connect(m_headers, &QTableWidget::itemChanged, this, [this] {
        if (m_syncing)
            return;
        updateTabLabels();
        scheduleDraftSave();
    });

    return page;
}

QWidget *HttpPanel::buildBodyTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *label = new QLabel(QStringLiteral("类型"), page);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@text-second;"));
    m_bodyMode = new ArrowCombo(page);
    m_bodyMode->addItem(QStringLiteral("无"), httpapi::bodyModeToken(httpapi::BodyMode::None));
    m_bodyMode->addItem(QStringLiteral("JSON"), httpapi::bodyModeToken(httpapi::BodyMode::Json));
    m_bodyMode->addItem(QStringLiteral("纯文本"), httpapi::bodyModeToken(httpapi::BodyMode::Text));
    m_bodyMode->addItem(QStringLiteral("XML"), httpapi::bodyModeToken(httpapi::BodyMode::Xml));
    m_bodyMode->addItem(QStringLiteral("表单 x-www-form-urlencoded"),
                        httpapi::bodyModeToken(httpapi::BodyMode::Form));
    m_bodyMode->setFixedWidth(210);
    m_bodyMode->setFixedHeight(32);

    m_formatJson = new QPushButton(QStringLiteral("格式化 JSON"), page);
    m_formatJson->setProperty("compact", true);
    m_formatJson->setToolTip(QStringLiteral("把当前请求体按 JSON 缩进重排；不合法时只提示不改内容"));

    row->addWidget(label);
    row->addWidget(m_bodyMode);
    row->addStretch(1);
    row->addWidget(m_formatJson);
    layout->addLayout(row);

    m_body = new QPlainTextEdit(page);
    m_body->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_body->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_body, 1);

    auto *hint = new QLabel(
        QStringLiteral("JSON / 文本 / XML 按 UTF-8 原样发送；表单会重新做一次 URL 编码，可以一行一个 key=value"),
        page);
    hint->setWordWrap(true);
    xtools::theme::applyStyle(hint, QStringLiteral("font-size:11px;color:@text-faint;"));
    layout->addWidget(hint);

    connect(m_bodyMode, &QComboBox::currentIndexChanged, this, [this] {
        updateBodyEditor();
        updateTabLabels();
        scheduleDraftSave();
    });
    connect(m_body, &QPlainTextEdit::textChanged, this, [this] {
        updateTabLabels();
        scheduleDraftSave();
    });
    connect(m_formatJson, &QPushButton::clicked, this, [this] {
        const QString pretty = httpapi::prettyJson(m_body->toPlainText().toUtf8());
        if (pretty.isEmpty()) {
            setHint(QStringLiteral("当前内容不是合法 JSON，没有改动"), true);
            return;
        }
        m_body->setPlainText(pretty);
        setHint(QStringLiteral("JSON 已格式化"));
    });

    return page;
}

QWidget *HttpPanel::buildAuthTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    auto *label = new QLabel(QStringLiteral("认证方式"), page);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@text-second;"));
    m_authMode = new ArrowCombo(page);
    m_authMode->addItem(QStringLiteral("无需认证"), httpapi::authModeToken(httpapi::AuthMode::None));
    m_authMode->addItem(QStringLiteral("Bearer Token"),
                        httpapi::authModeToken(httpapi::AuthMode::Bearer));
    m_authMode->addItem(QStringLiteral("Basic Auth"),
                        httpapi::authModeToken(httpapi::AuthMode::Basic));
    m_authMode->addItem(QStringLiteral("API Key"), httpapi::authModeToken(httpapi::AuthMode::ApiKey));
    m_authMode->setFixedWidth(180);
    m_authMode->setFixedHeight(32);
    row->addWidget(label);
    row->addWidget(m_authMode);
    row->addStretch(1);
    layout->addLayout(row);

    m_authStack = new QStackedWidget(page);
    layout->addWidget(m_authStack, 1);

    // 页 0：无认证
    {
        auto *empty = new QWidget(m_authStack);
        auto *hint = new QLabel(QStringLiteral("这个请求不附加认证信息。"), empty);
        xtools::theme::applyStyle(hint, QStringLiteral("font-size:12px;color:@text-faint;"));
        auto *box = new QVBoxLayout(empty);
        box->setContentsMargins(0, 4, 0, 0);
        box->addWidget(hint);
        box->addStretch(1);
        m_authStack->addWidget(empty);
    }

    // 页 1：Bearer
    {
        auto *formPage = new QWidget(m_authStack);
        auto *form = new QFormLayout(formPage);
        form->setContentsMargins(0, 4, 0, 0);
        form->setSpacing(8);
        m_token = new QLineEdit(formPage);
        m_token->setPlaceholderText(QStringLiteral("粘贴 access token，发送时加 Authorization: Bearer …"));
        form->addRow(QStringLiteral("Token"), m_token);
        m_authStack->addWidget(formPage);
    }

    // 页 2：Basic
    {
        auto *formPage = new QWidget(m_authStack);
        auto *form = new QFormLayout(formPage);
        form->setContentsMargins(0, 4, 0, 0);
        form->setSpacing(8);
        m_user = new QLineEdit(formPage);
        m_user->setPlaceholderText(QStringLiteral("用户名"));
        m_password = new QLineEdit(formPage);
        m_password->setEchoMode(QLineEdit::Password);
        m_password->setPlaceholderText(QStringLiteral("密码"));
        form->addRow(QStringLiteral("用户名"), m_user);
        form->addRow(QStringLiteral("密码"), m_password);
        m_authStack->addWidget(formPage);
    }

    // 页 3：API Key
    {
        auto *formPage = new QWidget(m_authStack);
        auto *form = new QFormLayout(formPage);
        form->setContentsMargins(0, 4, 0, 0);
        form->setSpacing(8);
        m_keyName = new QLineEdit(formPage);
        m_keyName->setPlaceholderText(QStringLiteral("键名，如 X-Api-Key"));
        m_keyValue = new QLineEdit(formPage);
        m_keyValue->setPlaceholderText(QStringLiteral("键值"));
        m_keyIn = new ArrowCombo(formPage);
        m_keyIn->addItem(QStringLiteral("请求头"), httpapi::keyInToken(httpapi::KeyIn::Header));
        m_keyIn->addItem(QStringLiteral("查询参数"), httpapi::keyInToken(httpapi::KeyIn::Query));
        m_keyIn->setFixedWidth(140);
        m_keyIn->setFixedHeight(32);
        form->addRow(QStringLiteral("键名"), m_keyName);
        form->addRow(QStringLiteral("键值"), m_keyValue);
        form->addRow(QStringLiteral("位置"), m_keyIn);
        m_authStack->addWidget(formPage);
    }

    auto *note = new QLabel(
        QStringLiteral("认证信息只随请求保存到本机存档，不会上传到任何第三方服务；共享电脑上别留生产密钥。"),
        page);
    note->setWordWrap(true);
    xtools::theme::applyStyle(note, QStringLiteral("font-size:11px;color:@text-faint;"));
    layout->addWidget(note);

    connect(m_authMode, &QComboBox::currentIndexChanged, this, [this] {
        updateAuthPage();
        updateTabLabels();
        scheduleDraftSave();
    });
    for (QLineEdit *edit : {m_token, m_user, m_password, m_keyName, m_keyValue}) {
        connect(edit, &QLineEdit::textChanged, this, &HttpPanel::scheduleDraftSave);
    }
    connect(m_keyIn, &QComboBox::currentIndexChanged, this, &HttpPanel::scheduleDraftSave);

    return page;
}

QWidget *HttpPanel::buildResponseArea()
{
    auto *area = new QWidget(this);
    auto *layout = new QVBoxLayout(area);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(8);
    m_status = new ElideLabel(area);
    m_status->setFullText(QStringLiteral("尚未发送请求"), Color::TextMuted);
    auto *copy = new QPushButton(QStringLiteral("复制响应"), area);
    copy->setProperty("compact", true);
    copy->setToolTip(QStringLiteral("复制当前看到的响应体（美化后的内容）"));
    bar->addWidget(m_status, 1);
    bar->addWidget(copy);
    layout->addLayout(bar);

    m_respTabs = new QTabWidget(area);
    m_respTabs->setDocumentMode(true);

    // ---- 响应体
    {
        auto *page = new QWidget(m_respTabs);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 8, 0, 0);
        pageLayout->setSpacing(8);

        auto *tools = new QHBoxLayout;
        tools->setSpacing(8);
        m_pretty = new QCheckBox(QStringLiteral("美化 JSON"), page);
        m_pretty->setEnabled(false);
        m_pretty->setToolTip(QStringLiteral("按缩进展开 JSON；取消勾选可看服务端返回的原始字节"));
        auto *typeHint = new QLabel(QStringLiteral("二进制响应只显示类型与体积，不往文本框里塞"), page);
        xtools::theme::applyStyle(typeHint, QStringLiteral("font-size:11px;color:@text-faint;"));
        tools->addWidget(m_pretty);
        tools->addStretch(1);
        tools->addWidget(typeHint);
        pageLayout->addLayout(tools);

        m_respBody = new QPlainTextEdit(page);
        m_respBody->setReadOnly(true);
        m_respBody->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_respBody->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        m_respBody->setPlaceholderText(QStringLiteral("响应内容会显示在这里"));
        pageLayout->addWidget(m_respBody, 1);

        m_respTabs->addTab(page, QStringLiteral("响应体"));
    }

    // ---- 响应头
    {
        auto *page = new QWidget(m_respTabs);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 8, 0, 0);
        m_respHeaders = makeTable(page, 2, true);
        m_respHeaders->setHorizontalHeaderLabels({QStringLiteral("响应头"), QStringLiteral("值")});
        m_respHeaders->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
        m_respHeaders->setColumnWidth(0, 240);
        m_respHeaders->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        pageLayout->addWidget(m_respHeaders, 1);
        m_respTabs->addTab(page, QStringLiteral("响应头"));
    }

    layout->addWidget(m_respTabs, 1);

    connect(copy, &QPushButton::clicked, this, &HttpPanel::copyResponse);
    connect(m_pretty, &QCheckBox::toggled, this, [this] { renderResponse(); });

    return area;
}

// ---------------------------------------------------------------- 请求 ↔ 界面

httpapi::Spec HttpPanel::gatherSpec() const
{
    httpapi::Spec spec;
    spec.method = m_method->currentText().trimmed().toUpper();

    // 地址栏与参数表可能暂时不同步（用户正在地址栏里改查询串、还没失焦）。
    // 以参数表为准，但表里一条都没有时退回地址栏解析出来的结果 —— 这样
    // 直接粘一条带 ?a=1 的地址按回车也不会把参数丢掉。
    const httpapi::UrlParts parts = httpapi::splitUrl(m_url->text());
    spec.url = parts.base;
    spec.params = collectKv(m_params);
    if (spec.params.isEmpty())
        spec.params = parts.params;

    spec.headers = collectKv(m_headers);
    spec.bodyMode = httpapi::bodyModeFromToken(m_bodyMode->currentData().toString());
    spec.body = m_body->toPlainText();

    const httpapi::AuthMode authMode =
        httpapi::authModeFromToken(m_authMode->currentData().toString());
    spec.auth.mode = authMode;
    if (authMode == httpapi::AuthMode::Bearer)
        spec.auth.token = m_token->text();
    else if (authMode == httpapi::AuthMode::Basic) {
        spec.auth.user = m_user->text();
        spec.auth.password = m_password->text();
    } else if (authMode == httpapi::AuthMode::ApiKey) {
        spec.auth.keyName = m_keyName->text().trimmed();
        spec.auth.keyValue = m_keyValue->text();
        spec.auth.keyIn = httpapi::keyInFromToken(m_keyIn->currentData().toString());
    }

    spec.name = m_currentName;
    return spec;
}

void HttpPanel::applySpec(const httpapi::Spec &spec)
{
    m_syncing = true;

    int methodIndex = m_method->findText(spec.method, Qt::MatchFixedString);
    if (methodIndex < 0) {
        // 从 cURL 导入的自定义动词（PROPFIND 之类）也要能显示，不能静默退回 GET。
        m_method->addItem(spec.method);
        methodIndex = m_method->count() - 1;
    }
    m_method->setCurrentIndex(methodIndex);

    m_currentName = spec.name;
    setKvTable(m_params, spec.params);
    setKvTable(m_headers, spec.headers);
    // 地址栏展示完整地址（含查询串），用户可以直接在这里改
    m_url->setText(httpapi::joinUrl(spec.url, spec.params));

    const int bodyIndex = m_bodyMode->findData(httpapi::bodyModeToken(spec.bodyMode));
    m_bodyMode->setCurrentIndex(bodyIndex < 0 ? 0 : bodyIndex);
    m_body->setPlainText(spec.body);

    const int authIndex = m_authMode->findData(httpapi::authModeToken(spec.auth.mode));
    m_authMode->setCurrentIndex(authIndex < 0 ? 0 : authIndex);
    m_token->setText(spec.auth.token);
    m_user->setText(spec.auth.user);
    m_password->setText(spec.auth.password);
    m_keyName->setText(spec.auth.keyName);
    m_keyValue->setText(spec.auth.keyValue);
    const int keyInIndex = m_keyIn->findData(httpapi::keyInToken(spec.auth.keyIn));
    m_keyIn->setCurrentIndex(keyInIndex < 0 ? 0 : keyInIndex);

    updateBodyEditor();
    updateAuthPage();
    updateTabLabels();

    m_syncing = false;
    scheduleDraftSave();
}

void HttpPanel::refreshParamsFromUrl()
{
    if (m_syncing)
        return;

    const httpapi::UrlParts parts = httpapi::splitUrl(m_url->text());
    // 地址栏当前的文本如果和「基地址 + 表格参数」拼出来的一致，说明改的是路径
    // 或协议之类的部分，参数表不需要重建（重建会把禁用行丢掉）。
    const QString rebuilt =
        httpapi::joinUrl(parts.base, collectKv(m_params), parts.fragment);
    if (rebuilt == m_url->text().trimmed())
        return;

    m_syncing = true;
    setKvTable(m_params, parts.params);
    m_syncing = false;

    updateTabLabels();
    scheduleDraftSave();
}

void HttpPanel::rewriteUrlFromParams()
{
    if (m_syncing)
        return;

    const httpapi::UrlParts parts = httpapi::splitUrl(m_url->text());
    const QString next = httpapi::joinUrl(parts.base, collectKv(m_params), parts.fragment);
    if (next == m_url->text())
        return;

    // 只改地址栏文本，不触发「地址栏被编辑」的后续逻辑（m_syncing 由
    // scheduleDraftSave 检查，textChanged 不会反过来重填表格）。
    const QSignalBlocker blocker(m_url);
    m_url->setText(next);

    scheduleDraftSave();
}

void HttpPanel::updateTabLabels()
{
    const int params = countEnabled(m_params);
    const int headers = countEnabled(m_headers);
    m_reqTabs->setTabText(0, params > 0 ? QStringLiteral("参数 (%1)").arg(params)
                                        : QStringLiteral("参数"));
    m_reqTabs->setTabText(1, headers > 0 ? QStringLiteral("请求头 (%1)").arg(headers)
                                         : QStringLiteral("请求头"));

    const httpapi::BodyMode bodyMode =
        httpapi::bodyModeFromToken(m_bodyMode->currentData().toString());
    const bool hasBody = bodyMode != httpapi::BodyMode::None
        && !m_body->toPlainText().trimmed().isEmpty();
    m_reqTabs->setTabText(2, hasBody ? QStringLiteral("请求体 ●") : QStringLiteral("请求体"));

    const bool hasAuth =
        httpapi::authModeFromToken(m_authMode->currentData().toString()) != httpapi::AuthMode::None;
    m_reqTabs->setTabText(3, hasAuth ? QStringLiteral("认证 ●") : QStringLiteral("认证"));
}

void HttpPanel::updateBodyEditor()
{
    const httpapi::BodyMode mode =
        httpapi::bodyModeFromToken(m_bodyMode->currentData().toString());
    m_body->setEnabled(mode != httpapi::BodyMode::None);
    m_formatJson->setEnabled(mode == httpapi::BodyMode::Json);

    switch (mode) {
    case httpapi::BodyMode::Json:
        m_body->setPlaceholderText(QStringLiteral("{\n  \"name\": \"cxk\"\n}"));
        break;
    case httpapi::BodyMode::Form:
        m_body->setPlaceholderText(
            QStringLiteral("name=cxk&from=launcher\n（也支持一行一个 key=value）"));
        break;
    case httpapi::BodyMode::Xml:
        m_body->setPlaceholderText(QStringLiteral("<root>\n  <name>cxk</name>\n</root>"));
        break;
    case httpapi::BodyMode::Text:
        m_body->setPlaceholderText(QStringLiteral("任意文本，按 UTF-8 发送"));
        break;
    case httpapi::BodyMode::None:
        m_body->setPlaceholderText(QStringLiteral("当前请求不带请求体"));
        break;
    }
}

void HttpPanel::updateAuthPage()
{
    m_authStack->setCurrentIndex(qMax(0, m_authMode->currentIndex()));
}

// ---------------------------------------------------------------- 集合 / 历史

void HttpPanel::refreshSavedList()
{
    m_savedList->clear();
    const QVector<httpapi::Spec> items = m_store.saved();
    for (const httpapi::Spec &spec : items) {
        auto *item = new QListWidgetItem(m_savedList);
        item->setData(kMethodRole, spec.method);
        item->setData(kTitleRole, spec.name);
        item->setData(kSubtitleRole, httpapi::effectiveUrl(spec));
        item->setData(kSpecRole, QString::fromUtf8(
                                     QJsonDocument(httpapi::specToJson(spec)).toJson(QJsonDocument::Compact)));
        item->setToolTip(spec.name + QLatin1Char('\n') + httpapi::effectiveUrl(spec));
    }
    m_savedEmpty->setVisible(items.isEmpty());
}

void HttpPanel::refreshHistoryList()
{
    m_historyList->clear();
    const QVector<httpapi::HistoryItem> items = m_store.history();
    for (const httpapi::HistoryItem &entry : items) {
        const httpapi::Spec &spec = entry.spec;
        const QUrl url = httpapi::toUrl(httpapi::effectiveUrl(spec));
        const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();

        auto *item = new QListWidgetItem(m_historyList);
        item->setData(kMethodRole, spec.method);
        item->setData(kTitleRole, path);
        item->setData(kSubtitleRole,
                      url.host() + QStringLiteral(" · ") + relativeTime(entry.at));
        item->setData(kSpecRole, QString::fromUtf8(
                                     QJsonDocument(httpapi::specToJson(spec)).toJson(QJsonDocument::Compact)));
        item->setToolTip(httpapi::effectiveUrl(spec) + QLatin1Char('\n')
                         + (entry.at.isValid() ? entry.at.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                               : QString()));
    }
    m_historyEmpty->setVisible(items.isEmpty());
}

void HttpPanel::saveCurrentToCollection()
{
    httpapi::Spec spec = gatherSpec();
    if (spec.url.trimmed().isEmpty()) {
        setHint(QStringLiteral("先填一个接口地址再保存"), true);
        return;
    }

    const QString suggestion = m_currentName.isEmpty() ? suggestName(spec) : m_currentName;
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("保存到集合"),
                                               QStringLiteral("给这个请求起个名字："),
                                               QLineEdit::Normal, suggestion, &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;

    spec.name = name;
    QVector<httpapi::Spec> items = m_store.saved();
    bool replaced = false;
    for (httpapi::Spec &item : items) {
        // 同名视为「更新」，不再追问：改完参数再保存是最高频的动作。
        if (item.name == name) {
            item = spec;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        items.append(spec);

    m_store.setSaved(items);
    m_currentName = name;
    refreshSavedList();
    setHint(replaced ? QStringLiteral("已更新集合项「%1」").arg(name)
                     : QStringLiteral("已保存到集合「%1」").arg(name));
}

void HttpPanel::deleteSavedItem(int row)
{
    QVector<httpapi::Spec> items = m_store.saved();
    if (row < 0 || row >= items.size())
        return;

    const QString name = items.at(row).name;
    items.removeAt(row);
    m_store.setSaved(items);
    if (m_currentName == name)
        m_currentName.clear();
    refreshSavedList();
    setHint(QStringLiteral("已从集合删除「%1」").arg(name));
}

void HttpPanel::deleteHistoryItem(int row)
{
    QVector<httpapi::HistoryItem> items = m_store.history();
    if (row < 0 || row >= items.size())
        return;
    items.removeAt(row);
    m_store.setHistory(items);
    refreshHistoryList();
    setHint(QStringLiteral("已删除这条历史"));
}

void HttpPanel::clearHistory()
{
    if (m_store.history().isEmpty()) {
        setHint(QStringLiteral("历史本来就是空的"));
        return;
    }
    m_store.setHistory({});
    refreshHistoryList();
    setHint(QStringLiteral("历史已清空（集合不受影响）"));
}

void HttpPanel::loadListItem(QListWidgetItem *item)
{
    if (!item)
        return;
    const httpapi::Spec spec = specFromItem(item);
    if (spec.url.trimmed().isEmpty())
        return;
    applySpec(spec);
    setHint(QStringLiteral("已载入：%1").arg(displayName(spec)));
}

void HttpPanel::showListMenu(const QPoint &pos, bool saved)
{
    QListWidget *list = saved ? m_savedList : m_historyList;
    QListWidgetItem *item = list->itemAt(pos);

    QMenu menu(this);
    QAction *load = nullptr;
    if (item)
        load = menu.addAction(QStringLiteral("载入这个请求"));

    QAction *copy = nullptr;
    if (item) {
        copy = menu.addAction(QStringLiteral("复制为 cURL"));
        menu.addSeparator();
    }

    QAction *remove = menu.addAction(saved ? QStringLiteral("从集合中删除")
                                           : QStringLiteral("删除这条历史"));
    remove->setEnabled(item != nullptr);

    QAction *clear = nullptr;
    if (!saved) {
        menu.addSeparator();
        clear = menu.addAction(QStringLiteral("清空全部历史"));
    }

    QAction *chosen = menu.exec(list->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == load && item) {
        loadListItem(item);
    } else if (chosen == copy && item) {
        QGuiApplication::clipboard()->setText(httpapi::toCurl(specFromItem(item)));
        setHint(QStringLiteral("cURL 已复制到剪贴板"));
    } else if (chosen == remove && item) {
        const int row = list->row(item);
        if (saved)
            deleteSavedItem(row);
        else
            deleteHistoryItem(row);
    } else if (chosen == clear) {
        clearHistory();
    }
}

// ---------------------------------------------------------------- 发送

void HttpPanel::send()
{
    // 请求进行中，这个按钮就是「取消」。
    if (m_reply) {
        m_reply->abort();
        return;
    }

    // 地址栏里刚敲完但还没失焦的查询串先同步进表格，否则这次发送会漏参数。
    refreshParamsFromUrl();

    const httpapi::Spec spec = gatherSpec();
    if (spec.url.trimmed().isEmpty()) {
        setHint(QStringLiteral("先填请求地址，例如 https://httpbin.org/get"), true);
        m_url->setFocus();
        return;
    }

    const QString fullUrl = httpapi::effectiveUrl(spec);
    const QUrl url = httpapi::toUrl(fullUrl);
    if (!url.isValid() || url.host().isEmpty()) {
        setHint(QStringLiteral("地址不合法：%1").arg(fullUrl), true);
        return;
    }

    QNetworkRequest request(url);
    // 自动跟随 3xx：接口调试时最常见的场景是 http → https 或末尾斜杠重定向。
    // NoLessSafe 不允许 https → http 降级，避免把认证头带到明文连接上。
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMs);

    const auto headers = httpapi::effectiveHeaders(spec);
    for (const auto &header : headers) {
        // setRawHeader 不做校验，重复键后写的覆盖先写的；用户自己写的头在前，
        // 认证自动生成的头在后 —— 也就是说认证页的配置优先级高于请求头表。
        request.setRawHeader(header.first.toUtf8(), header.second.toUtf8());
    }

    const QByteArray payload = httpapi::bodyPayload(spec);
    m_pendingSpec = spec;
    m_respRaw.clear();
    m_respContentType.clear();
    m_respIsJson = false;
    m_respBody->clear();
    m_respHeaders->setRowCount(0);
    m_pretty->setEnabled(false);
    m_pretty->setChecked(false);
    m_respTabs->setTabText(1, QStringLiteral("响应头"));

    m_reply = m_nam->sendCustomRequest(request, spec.method.toUtf8(), payload);
    m_elapsed.start();
    m_send->setText(QStringLiteral("取消"));
    setStatus(QStringLiteral("请求中…"), -1);
    setHint(QStringLiteral("%1 %2").arg(spec.method, fullUrl));

    connect(m_reply, &QNetworkReply::finished, this, &HttpPanel::onReplyFinished);
}

void HttpPanel::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    if (reply != m_reply) {
        // 理论上不会走到：同一时间只有一个在途请求。留着防御，避免误用别的 reply。
        reply->deleteLater();
        return;
    }

    m_reply = nullptr;
    const qint64 elapsed = m_elapsed.elapsed();
    m_send->setText(QStringLiteral("发送"));

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    if (reason.isEmpty())
        reason = reasonPhrase(status);

    const QByteArray data = reply->readAll();
    m_respRaw = data;
    m_respContentType = QString::fromUtf8(reply->rawHeader("Content-Type")).trimmed();
    fillResponseHeaders(reply->rawHeaderPairs());

    if (status == 0) {
        // 连 HTTP 状态码都没拿到：DNS、拒绝连接、TLS、超时或被取消。
        m_respIsJson = false;
        m_pretty->setEnabled(false);
        m_pretty->setChecked(false);

        QStringList lines;
        lines << QStringLiteral("请求没有拿到 HTTP 响应。");
        lines << QString();
        lines << QStringLiteral("地址：%1").arg(httpapi::effectiveUrl(m_pendingSpec));
        lines << QStringLiteral("原因：%1").arg(reply->errorString());
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            lines << QStringLiteral("说明：主动取消，或 30 秒内没有收到任何数据被判定超时。");
        } else {
            lines << QStringLiteral("排查顺序：服务是否在运行 → 端口和路径是否正确 → 是否需要代理或证书信任。");
        }
        m_respBody->setPlainText(lines.join(QLatin1Char('\n')));
        setStatus(QStringLiteral("请求失败 · %1").arg(httpapi::formatDuration(elapsed)), 0);
        setHint(QStringLiteral("请求失败：") + reply->errorString(), true);
    } else {
        m_respIsJson = httpapi::isJsonContentType(m_respContentType)
            || (m_respContentType.isEmpty() && !httpapi::prettyJson(data).isEmpty()
                && !data.trimmed().isEmpty());
        m_pretty->setEnabled(m_respIsJson);
        m_pretty->setChecked(m_respIsJson);
        renderResponse();

        QString summary = QStringLiteral("%1 %2 · %3 · %4")
                              .arg(status)
                              .arg(reason)
                              .arg(httpapi::formatDuration(elapsed), httpapi::formatBytes(data.size()));
        if (!m_respContentType.isEmpty())
            summary += QStringLiteral(" · ") + m_respContentType;
        setStatus(summary, status);

        m_respTabs->setTabText(1, QStringLiteral("响应头 (%1)").arg(m_respHeaders->rowCount()));
        setHint(QStringLiteral("响应已收到；右键侧栏历史可以复制为 cURL"));
    }

    // 历史记录的是「发出去的请求」，与响应成败无关：失败的地址往往更需要回看。
    m_store.addHistory(m_pendingSpec);
    refreshHistoryList();

    reply->deleteLater();
}

void HttpPanel::renderResponse()
{
    if (!httpapi::isTextualContentType(m_respContentType)) {
        m_respBody->setPlainText(
            QStringLiteral("二进制响应（%1），不在文本视图里展开。\n\nContent-Type: %2")
                .arg(httpapi::formatBytes(m_respRaw.size()),
                     m_respContentType.isEmpty() ? QStringLiteral("未知") : m_respContentType));
        return;
    }

    QString text = QString::fromUtf8(m_respRaw);
    if (m_respIsJson && m_pretty && m_pretty->isChecked()) {
        const QString pretty = httpapi::prettyJson(m_respRaw);
        if (!pretty.isEmpty())
            text = pretty;
    }
    if (text.isEmpty())
        text = QStringLiteral("(空响应体)");

    m_respBody->setPlainText(text);
    m_respBody->moveCursor(QTextCursor::Start);
}

void HttpPanel::fillResponseHeaders(const QList<QPair<QByteArray, QByteArray>> &headers)
{
    m_respHeaders->setRowCount(0);
    for (const auto &header : headers) {
        const int row = m_respHeaders->rowCount();
        m_respHeaders->insertRow(row);
        auto *name = new QTableWidgetItem(QString::fromLatin1(header.first));
        auto *value = new QTableWidgetItem(QString::fromUtf8(header.second));
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_respHeaders->setItem(row, 0, name);
        m_respHeaders->setItem(row, 1, value);
    }
}

// ---------------------------------------------------------------- 小工具

void HttpPanel::setHint(const QString &text, bool error)
{
    m_hint->setText(text);
    // 提示行颜色每次重算：错误用 danger，普通提示用最弱的灰。
    xtools::theme::applyStyle(
        m_hint, QStringLiteral("font-size:12px;color:@%1;")
                    .arg(xtools::theme::tokenName(error ? Color::DangerText : Color::TextFaint)));
}

void HttpPanel::setStatus(const QString &text, int statusCode)
{
    Color token = Color::TextMuted;
    if (statusCode == -1)
        token = Color::AccentText;
    else if (statusCode >= 200 && statusCode < 300)
        token = Color::SuccessText;
    else if (statusCode >= 300 && statusCode < 400)
        token = Color::WarnText;
    else if (statusCode >= 400)
        token = Color::DangerText;

    m_status->setFullText(text, token);
}

void HttpPanel::scheduleDraftSave()
{
    if (m_syncing || !m_draftTimer)
        return;
    m_draftTimer->start();
}

void HttpPanel::saveDraftNow()
{
    if (m_syncing)
        return;
    m_store.setDraft(gatherSpec());
}

void HttpPanel::copyResponse()
{
    const QString text = m_respBody->toPlainText();
    if (text.isEmpty()) {
        setHint(QStringLiteral("还没有响应可复制"), true);
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    setHint(QStringLiteral("响应已复制到剪贴板"));
}

void HttpPanel::copyCurl()
{
    QGuiApplication::clipboard()->setText(httpapi::toCurl(gatherSpec()));
    setHint(QStringLiteral("cURL 已复制，可直接粘进终端"));
}

void HttpPanel::importCurl()
{
    const QString text = QGuiApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) {
        setHint(QStringLiteral("剪贴板是空的"), true);
        return;
    }

    const httpapi::CurlParseResult result = httpapi::parseCurl(text);
    if (!result.ok) {
        setHint(QStringLiteral("没解析出请求：") + result.error, true);
        return;
    }

    applySpec(result.spec);
    m_reqTabs->setCurrentIndex(0);
    setHint(QStringLiteral("已从剪贴板导入，确认地址与参数后再发送"));
}

#include "ProcMonPanel.h"
#include "ThemePalette.h"   // 自绘控件用的颜色令牌

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

/// 采样间隔。1 秒会把 CPU 用在"看数字跳动"上（每次要枚举几百个进程、还要扫
/// 端口与句柄），3 秒又会让"刚打开一个大程序"的反馈太迟钝 —— 2 秒是折中。
constexpr int kRefreshIntervalMs = 2000;

/// 自动释放的最小间隔。工作集被刷掉后进程会立刻换页回来，
/// 连续刷等于反复踩同一块地，只会让系统更慢。
constexpr int kAutoCooldownSecs = 30;

/// 手动"释放内存"的下限与批量上限：小于 64 MB 的进程榨不出量，
/// 一次刷几十个又太激进（每个被刷的进程都会重新缺页）。
constexpr quint64 kManualMinBytes = 64ull * 1024 * 1024;
constexpr int kManualMaxCount = 40;

/// 自动释放更保守：只动 128 MB 以上的、最多 6 个。
constexpr quint64 kAutoMinBytes = 128ull * 1024 * 1024;
constexpr int kAutoMaxCount = 6;

/// 表格列号。散在代码里写 2 / 3 / 4 太容易错位，统一在这里对一次。
enum Column
{
    ColName = 0,
    ColPid,
    ColCpu,
    ColMemory,
    ColDisk,
    ColNet,
    ColPort
};

/// 阈值下拉框右侧的小三角。
///
/// 为什么不交给 qss：给 `QComboBox::drop-down` 写规则能去掉原生那条甩到圆角框外的
/// 深色 L 形边框，但 Qt 同时就不画默认箭头了；改用 `::down-arrow { image }` 会把图片
/// 重复画到文字后面，改成 `background-image` 又整片平铺。自绘一个透明子控件最省事，
/// 位置由 ProcMonPanel::eventFilter 跟着下拉框尺寸摆。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(10, 8);
        setAttribute(Qt::WA_TransparentForMouseEvents);   // 点击穿透给下拉框
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        // 自绘小三角跟主题走：写死浅灰在深色底上几乎看不见
        painter.setBrush(isEnabled() ? xtools::theme::color(xtools::theme::Color::TextFaint)
                                     : xtools::theme::color(xtools::theme::Color::TextDisabled));
        QPainterPath path;
        path.moveTo(0.5, 2.0);
        path.lineTo(9.5, 2.0);
        path.lineTo(5.0, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
    }
};

/// 表格行高。qss 的 ::item padding 只管左右，竖向高度由 delegate 的 sizeHint
/// 决定，所以这里补一个最小高度，避免默认 ~20px 的行挤成一团。
class RowHeightDelegate : public QStyledItemDelegate
{
public:
    explicit RowHeightDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), 30));
        return size;
    }
};

/// 单个进程占系统物理内存的百分比文案。
///
/// 小进程会算出 0.03% 这种，直接格式化成 "0.0%" 看着像没占用，
/// 所以小于 0.05% 的一律显示 "<0.1%"。
QString rowPercentText(quint64 workingSet, quint64 totalBytes)
{
    if (totalBytes == 0)
        return QStringLiteral("—");
    const double percent = 100.0 * static_cast<double>(workingSet) / static_cast<double>(totalBytes);
    if (workingSet > 0 && percent < 0.05)
        return QStringLiteral("<0.1%");
    return QStringLiteral("%1%").arg(percent, 0, 'f', 1);
}

/// 改动态属性后必须重新 polish，否则 qss 里 [state="warn"] 这类规则不会立刻生效。
void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

/// 概览卡等级 → qss 用的字符串（对应 #procPercent[level="..."] 与 #procBar[level="..."]）。
QString levelName(int level)
{
    if (level >= 2)
        return QStringLiteral("danger");
    if (level == 1)
        return QStringLiteral("warn");
    return QStringLiteral("normal");
}

/// 网络列的悬浮说明。按进程统计网络流量在三大系统都没有免管理权限的公开接口，
/// 这一列固定显示 "—"，把原因写在这里比让人猜更好。
QString netColumnTip()
{
    return QStringLiteral("按进程统计网络流量需要内核级接口"
                          "（Windows 需 ETW 且要管理员权限，Linux 只能按网络命名空间统计），"
                          "因此这一列不提供数字；整机速率见上方概览卡");
}

} // namespace

ProcMonPanel::ProcMonPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐，视觉上像同一栏
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(12);

    // ---------------------------------------------------------------- 概览卡
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("procCard"));
    auto *cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(18, 12, 18, 12);
    cardLayout->setSpacing(18);

    auto *percentBox = new QVBoxLayout;
    percentBox->setSpacing(0);
    auto *caption = new QLabel(QStringLiteral("内存占用"), card);
    caption->setObjectName(QStringLiteral("procCaption"));
    m_percent = new QLabel(QStringLiteral("—"), card);
    m_percent->setObjectName(QStringLiteral("procPercent"));
    percentBox->addWidget(caption);
    percentBox->addWidget(m_percent);
    percentBox->addStretch(1);
    cardLayout->addLayout(percentBox);

    auto *barBox = new QVBoxLayout;
    barBox->setSpacing(8);
    m_used = new QLabel(QStringLiteral("正在读取…"), card);
    m_used->setObjectName(QStringLiteral("procUsed"));
    m_bar = new QProgressBar(card);
    m_bar->setObjectName(QStringLiteral("procBar"));
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(6);
    barBox->addStretch(1);
    barBox->addWidget(m_used);
    barBox->addWidget(m_bar);
    barBox->addStretch(1);
    cardLayout->addLayout(barBox, 1);

    auto *statBox = new QVBoxLayout;
    statBox->setSpacing(4);
    m_avail = new QLabel(QStringLiteral("可用 —"), card);
    m_avail->setObjectName(QStringLiteral("procAvail"));
    m_net = new QLabel(QStringLiteral("网络 ↑0 B/s ↓0 B/s"), card);
    m_net->setObjectName(QStringLiteral("procNet"));
    m_released = new QLabel(QStringLiteral("本次已释放 0 B"), card);
    m_released->setObjectName(QStringLiteral("procReleased"));
    statBox->addStretch(1);
    statBox->addWidget(m_avail, 0, Qt::AlignRight);
    statBox->addWidget(m_net, 0, Qt::AlignRight);
    statBox->addWidget(m_released, 0, Qt::AlignRight);
    statBox->addStretch(1);
    cardLayout->addLayout(statBox);

    layout->addWidget(card);

    // ---------------------------------------------------------------- 操作条
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("procSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索进程名 / PID / 端口"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(260);
    toolbar->addWidget(m_search, 1);

    const bool canTrim = procprobe::canTrimOtherProcesses();

    m_auto = new QCheckBox(QStringLiteral("自动释放"), this);
    m_auto->setObjectName(QStringLiteral("procAuto"));
    // 默认就开着：这条开关是"内存吃紧时自动收拾一下"，用户不用先想到它
    m_auto->setChecked(canTrim);
    m_auto->setEnabled(canTrim);
    m_auto->setToolTip(canTrim
        ? QStringLiteral("内存占用超过设定阈值时，自动回收占用最大的几个进程"
                         "（两次之间至少隔 %1 秒）").arg(kAutoCooldownSecs)
        : procprobe::unsupportedTrimReason());
    toolbar->addWidget(m_auto);

    m_threshold = new QComboBox(this);
    m_threshold->setObjectName(QStringLiteral("procThreshold"));
    // 文案按"占用"给（任务管理器的说法），与用户直觉一致；判定时换算成可用阈值
    for (const int used : {90, 85, 80, 75})
        m_threshold->addItem(QStringLiteral("占用 ≥ %1%").arg(used), used);
    m_threshold->setCurrentIndex(2);   // 默认 80%
    m_threshold->setEnabled(canTrim);
    toolbar->addWidget(m_threshold);
    m_comboArrow = new ComboArrow(m_threshold);
    m_comboArrow->setObjectName(QStringLiteral("procComboArrow"));
    m_threshold->installEventFilter(this);

    toolbar->addStretch(1);

    m_releaseSelected = new QPushButton(QStringLiteral("释放选中"), this);
    m_releaseSelected->setObjectName(QStringLiteral("procReleaseSelected"));
    m_releaseSelected->setEnabled(false);
    toolbar->addWidget(m_releaseSelected);

    m_releaseAll = new QPushButton(QStringLiteral("释放内存"), this);
    m_releaseAll->setObjectName(QStringLiteral("procReleaseAll"));
    m_releaseAll->setProperty("accent", true);   // 主操作
    m_releaseAll->setEnabled(canTrim);
    m_releaseAll->setToolTip(canTrim
        ? QStringLiteral("把 %1 以上进程的工作集刷入页面文件；回收量显示在下方状态栏").arg(
              proctext::formatBytes(kManualMinBytes))
        : procprobe::unsupportedTrimReason());
    toolbar->addWidget(m_releaseAll);

    layout->addLayout(toolbar);

    // ---------------------------------------------------------------- 进程表
    m_table = new QTreeWidget(this);
    m_table->setObjectName(QStringLiteral("procTable"));
    m_table->setColumnCount(7);
    m_table->setHeaderLabels({QStringLiteral("进程"), QStringLiteral("PID"),
                              QStringLiteral("CPU"), QStringLiteral("内存"),
                              QStringLiteral("磁盘"), QStringLiteral("网络"),
                              QStringLiteral("端口")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setItemDelegate(new RowHeightDelegate(m_table));
    m_table->headerItem()->setToolTip(ColNet, netColumnTip());

    QHeaderView *header = m_table->header();
    header->setFixedHeight(30);
    header->setStretchLastSection(false);
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    header->setSectionResizeMode(ColName, QHeaderView::Stretch);
    for (const int column : {ColPid, ColCpu, ColMemory, ColDisk, ColNet, ColPort})
        header->setSectionResizeMode(column, QHeaderView::Fixed);
    // 宽度按"最长的真实数据"定：PID 要装下 5 位数（"34728"），磁盘要装下
    // "123 MB/s"，端口要装下三个端口（"8080, 443, 80"）——不然一列一个省略号
    header->resizeSection(ColPid, 78);
    header->resizeSection(ColCpu, 68);
    header->resizeSection(ColMemory, 92);
    header->resizeSection(ColDisk, 92);
    header->resizeSection(ColNet, 72);
    header->resizeSection(ColPort, 142);
    for (const int column : {ColPid, ColCpu, ColMemory, ColDisk, ColNet})
        m_table->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    header->setSortIndicator(ColMemory, Qt::DescendingOrder);

    layout->addWidget(m_table, 1);

    // ---------------------------------------------------------------- 底部
    auto *footer = new QHBoxLayout;
    footer->setSpacing(12);
    m_status = new QLabel(QStringLiteral("正在读取…"), this);
    m_status->setObjectName(QStringLiteral("procStatus"));
    auto *hint = new QLabel(QStringLiteral("右键进程：释放内存 / 结束 / 重启 / 打开所在位置"),
                            this);
    hint->setObjectName(QStringLiteral("procHint"));
    hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    footer->addWidget(m_status, 1);
    footer->addWidget(hint, 0);
    layout->addLayout(footer);

    // ---------------------------------------------------------------- 连接
    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &ProcMonPanel::refresh);

    // 搜索只重排列表，不重新扫进程：扫一次要开几百个句柄，边打字边扫会卡顿
    connect(m_search, &QLineEdit::textChanged, this, [this] {
        rebuildTable();
        if (m_statusHoldUntil.isValid() && QDateTime::currentDateTime() < m_statusHoldUntil)
            return;
        const int shown = m_table->topLevelItemCount();
        setStatus(shown == m_processes.size()
                      ? QStringLiteral("共 %1 个进程").arg(m_processes.size())
                      : QStringLiteral("匹配 %1 / %2 个进程").arg(shown).arg(m_processes.size()),
                  false, 0);
    });

    connect(m_auto, &QCheckBox::toggled, this, [this](bool checked) {
        // 平台不支持回收时不要留下一个"看着开着、其实什么都不做"的开关
        if (checked && !procprobe::canTrimOtherProcesses()) {
            m_auto->setChecked(false);
            setStatus(procprobe::unsupportedTrimReason(), true, 8);
            return;
        }
        m_threshold->setEnabled(checked);
        if (checked)
            refresh();   // 已经在高占用状态时立刻生效，不用等下一轮
    });

    connect(m_threshold, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (m_auto->isChecked())
            refresh();
    });

    connect(m_releaseAll, &QPushButton::clicked, this, &ProcMonPanel::releaseAll);
    connect(m_releaseSelected, &QPushButton::clicked, this, &ProcMonPanel::releaseSelected);
    connect(m_table, &QTreeWidget::itemSelectionChanged, this, &ProcMonPanel::updateSelectionState);
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, &ProcMonPanel::showContextMenu);
    connect(header, &QHeaderView::sectionClicked, this, &ProcMonPanel::applySortColumn);

    updateSummary();
    updateSelectionState();
}

void ProcMonPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
    m_timer->start();

    // 延后一拍再抢焦点：同步设会被宿主的焦点链覆盖（同 JsonFmtPanel）
    QTimer::singleShot(0, this, [this] {
        m_search->setFocus();
        m_search->selectAll();
    });
}

void ProcMonPanel::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    QWidget::hideEvent(event);
}

bool ProcMonPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_threshold && m_comboArrow) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show: {
            // 与 qss 里 ::drop-down 的 18px 箭头区对齐；窗口拉伸、字体缩放后都要重摆
            const QRect box = m_threshold->rect();
            m_comboArrow->move(box.width() - 18 + 4, (box.height() - m_comboArrow->height()) / 2);
            break;
        }
        case QEvent::EnabledChange:
            m_comboArrow->update();   // 禁用时箭头跟着变淡
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ProcMonPanel::refresh()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // 第一次采样没有基准，elapsed 记 0（computeRates 会因此返回空表）
    const qint64 elapsed = m_lastSampleMs > 0 ? nowMs - m_lastSampleMs : 0;

    const procprobe::SystemMemory system = procprobe::readSystemMemory();
    if (system.valid)
        m_sys = system;

    const procprobe::SystemNet net = procprobe::readSystemNet();
    QVector<procprobe::ProcessInfo> processes = procprobe::listProcesses();

    // 速率只在两次采样都有意义时才算：释放内存后会立刻 refresh 一次，
    // 那种 20ms 的间隔算出来的数字全是噪音
    if (elapsed >= 500) {
        m_rates = proctext::computeRates(m_previous, processes, elapsed,
                                        QThread::idealThreadCount());
        if (net.valid && m_netCounters.valid) {
            const quint64 rx = net.rxBytes > m_netCounters.rxBytes
                ? net.rxBytes - m_netCounters.rxBytes
                : 0;
            const quint64 tx = net.txBytes > m_netCounters.txBytes
                ? net.txBytes - m_netCounters.txBytes
                : 0;
            const double seconds = static_cast<double>(elapsed) / 1000.0;
            m_net->setText(QStringLiteral("网络 ↑%1 ↓%2")
                               .arg(proctext::formatRate(tx / seconds),
                                    proctext::formatRate(rx / seconds)));
        }
    }
    if (net.valid) {
        m_netCounters = net;
    } else {
        // 读不到网卡计数（少见，但权限受限的容器里会这样）：给个钉子文案，
        // 别让概览卡一直挂着上一次的速率或初始的 0
        m_net->setText(QStringLiteral("网络 —"));
    }

    m_processes = processes;
    m_previous = processes;
    m_lastSampleMs = nowMs;

    updateSummary();
    rebuildTable();
    maybeAutoRelease();

    // 刚显示过操作结果时别盖掉它（用户正看着"回收了 1.2 GB"）
    if (m_statusHoldUntil.isValid() && QDateTime::currentDateTime() < m_statusHoldUntil)
        return;

    const int shown = m_table->topLevelItemCount();
    QString text = QStringLiteral("共 %1 个进程").arg(m_processes.size());
    if (shown != m_processes.size())
        text += QStringLiteral("，匹配 %1 个").arg(shown);
    text += QStringLiteral(" · 每 %1 秒刷新").arg(kRefreshIntervalMs / 1000);
    setStatus(text, false, 0);
}

void ProcMonPanel::updateSummary()
{
    if (!m_sys.valid || m_sys.totalBytes == 0) {
        m_percent->setText(QStringLiteral("—"));
        m_used->setText(QStringLiteral("读不到本机内存信息"));
        m_avail->setText(QStringLiteral("可用 —"));
        m_bar->setValue(0);
        return;
    }

    const quint64 avail = qMin(m_sys.availBytes, m_sys.totalBytes);
    const double used = proctext::usedPercent(m_sys);

    m_percent->setText(proctext::formatPercent(used));
    m_used->setText(QStringLiteral("已用 %1 / 共 %2")
                        .arg(proctext::formatBytes(m_sys.totalBytes - avail),
                             proctext::formatBytes(m_sys.totalBytes)));
    m_avail->setText(QStringLiteral("可用 %1").arg(proctext::formatBytes(avail)));
    m_released->setText(QStringLiteral("本次已释放 %1").arg(
        proctext::formatBytes(m_releasedTotal)));
    m_bar->setValue(qRound(used));

    const int level = proctext::levelFor(used);
    if (level == m_lastLevel)
        return;
    m_lastLevel = level;
    const QString name = levelName(level);
    for (QWidget *widget : {static_cast<QWidget *>(m_percent), static_cast<QWidget *>(m_bar)}) {
        widget->setProperty("level", name);
        repolish(widget);
    }
}

void ProcMonPanel::rebuildTable()
{
    const QVector<procprobe::ProcessInfo> rows = proctext::filterAndSort(
        m_processes, m_search->text().trimmed(), m_rates, m_sortKey, m_sortDescending);

    QVector<quint32> pids;
    pids.reserve(rows.size());
    for (const procprobe::ProcessInfo &process : rows)
        pids.append(process.pid);

    // 顺序没变就只改文字：整表重建每 2 秒闪一下，而且会打断鼠标悬停/拖选
    if (pids == m_rowPids && m_table->topLevelItemCount() == rows.size()) {
        for (int i = 0; i < rows.size(); ++i)
            updateRow(m_table->topLevelItem(i), rows.at(i));
        updateSelectionState();
        return;
    }
    rebuildTableFully(rows);
    m_rowPids = pids;
    updateSelectionState();
}

void ProcMonPanel::rebuildTableFully(const QVector<procprobe::ProcessInfo> &rows)
{
    const quint32 keepPid = selectedPid();
    const int scroll = m_table->verticalScrollBar()->value();

    m_table->setUpdatesEnabled(false);
    m_table->clear();
    QTreeWidgetItem *restore = nullptr;
    for (const procprobe::ProcessInfo &process : rows) {
        auto *item = new QTreeWidgetItem(m_table);
        item->setData(ColName, Qt::UserRole, process.pid);
        for (const int column : {ColPid, ColCpu, ColMemory, ColDisk, ColNet})
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        updateRow(item, process);
        if (process.pid == keepPid)
            restore = item;
    }
    m_table->setUpdatesEnabled(true);

    if (restore)
        m_table->setCurrentItem(restore);
    // 滚动位置按新的最大值夹一下：进程数变少时直接设旧值会被吞掉
    m_table->verticalScrollBar()->setValue(
        qMin(scroll, m_table->verticalScrollBar()->maximum()));
}

void ProcMonPanel::updateRow(QTreeWidgetItem *item, const procprobe::ProcessInfo &process)
{
    if (item->text(ColName) != process.name)
        item->setText(ColName, process.name);

    const QString pid = QString::number(process.pid);
    if (item->text(ColPid) != pid)
        item->setText(ColPid, pid);

    const auto rateIt = m_rates.constFind(process.pid);
    const proctext::ProcRates rates = rateIt == m_rates.constEnd()
        ? proctext::ProcRates{}
        : rateIt.value();

    // CPU：第一轮采样还没有基准，显示 "—" 而不是 0%（后者会让人以为进程没在跑）
    const QString cpu = (process.countersReadable && rates.valid)
        ? proctext::formatCpu(rates.cpuPercent)
        : QStringLiteral("—");
    if (item->text(ColCpu) != cpu)
        item->setText(ColCpu, cpu);

    const QString memory = process.memoryReadable
        ? proctext::formatBytes(process.workingSet)
        : QStringLiteral("—");
    if (item->text(ColMemory) != memory)
        item->setText(ColMemory, memory);

    // 磁盘：平台没接口、进程读不到计数器、或还没算出速率，统统显示 "—"
    QString disk = QStringLiteral("—");
    if (procprobe::canReadDiskCounters() && process.countersReadable && rates.valid)
        disk = proctext::formatRate(rates.diskBytesPerSec);
    if (item->text(ColDisk) != disk)
        item->setText(ColDisk, disk);

    // 网络：见 netColumnTip()，这一列固定给 "—"，数字给整机
    if (item->text(ColNet) != QStringLiteral("—")) {
        item->setText(ColNet, QStringLiteral("—"));
        item->setToolTip(ColNet, netColumnTip());
    }

    // 列里只放两个端口就够看出"这进程在听什么"，其余折成 +N，完整列表在悬浮提示里
    const QString ports = proctext::formatPorts(process.ports, 2);
    if (item->text(ColPort) != ports)
        item->setText(ColPort, ports);

    // 悬浮提示把一列装不下的信息补齐：路径、内存占比、完整端口表
    QString tip;
    if (!process.exePath.isEmpty())
        tip = process.exePath;
    else if (!process.memoryReadable)
        tip = QStringLiteral("受保护的系统进程，读不到路径与内存信息");
    if (process.memoryReadable && m_sys.valid && m_sys.totalBytes > 0) {
        if (!tip.isEmpty())
            tip += QLatin1Char('\n');
        tip += QStringLiteral("内存 %1（占系统 %2）")
                   .arg(proctext::formatBytes(process.workingSet),
                        rowPercentText(process.workingSet, m_sys.totalBytes));
    }
    if (!process.ports.isEmpty()) {
        if (!tip.isEmpty())
            tip += QLatin1Char('\n');
        tip += QStringLiteral("监听端口：%1").arg(proctext::formatPorts(process.ports, 12));
    }
    if (item->toolTip(ColName) != tip)
        item->setToolTip(ColName, tip);

    if (item->data(ColName, Qt::UserRole).toUInt() != process.pid)
        item->setData(ColName, Qt::UserRole, process.pid);
}

void ProcMonPanel::releaseAll()
{
    // 自己（面板所在进程）不参与：刷自己没收益，还会让刚画好的界面重新缺页
    const QVector<quint32> pids = proctext::pickReleaseCandidates(
        m_processes, kManualMinBytes, kManualMaxCount, QCoreApplication::applicationPid());
    releaseProcesses(pids, false);
}

void ProcMonPanel::releaseSelected()
{
    const quint32 pid = selectedPid();
    if (pid == 0)
        return;
    // 用户明确点了这一行就不再卡 64 MB 门槛：小进程也可以手动刷
    releaseProcesses(QVector<quint32>{pid}, false);
}

void ProcMonPanel::releaseProcesses(const QVector<quint32> &pids, bool automatic)
{
    if (!procprobe::canTrimOtherProcesses()) {
        setStatus(procprobe::unsupportedTrimReason(), true, 8);
        return;
    }
    if (pids.isEmpty()) {
        setStatus(QStringLiteral("没有可释放的进程（都小于 %1）").arg(
                      proctext::formatBytes(kManualMinBytes)),
                  false, 4);
        return;
    }

    // m_releasing 挡住 refresh() 里的自动释放：否则手动释放完这一刷又触发一轮
    m_releasing = true;
    quint64 freed = 0;
    int succeeded = 0;
    int denied = 0;
    QString firstError;
    for (const quint32 pid : pids) {
        const procprobe::TrimOutcome outcome = procprobe::trimProcess(pid);
        if (outcome.ok) {
            ++succeeded;
            freed += outcome.freedBytes();
        } else {
            ++denied;
            if (firstError.isEmpty())
                firstError = outcome.error;
        }
    }
    m_releasedTotal += freed;

    // 先刷新再写状态：refresh() 会把状态栏改回"共 N 个进程"
    refresh();
    m_releasing = false;

    QString text = automatic ? QStringLiteral("自动释放：") : QString();
    if (succeeded == 0) {
        text += QStringLiteral("没有释放成功（%1）").arg(firstError);
        setStatus(text, true, 8);
    } else {
        text += QStringLiteral("回收 %1（%2 个进程）")
                    .arg(proctext::formatBytes(freed))
                    .arg(succeeded);
        if (denied > 0)
            text += QStringLiteral("，%1 个无权限").arg(denied);
        setStatus(text, false, 6);
    }
}

void ProcMonPanel::maybeAutoRelease()
{
    if (m_releasing || !m_auto->isChecked() || !m_sys.valid)
        return;

    const QDateTime now = QDateTime::currentDateTime();
    // 下拉里存的是"占用阈值"，判定函数吃"可用阈值"，换算一次
    const int availThreshold = proctext::availableThreshold(m_threshold->currentData().toInt());
    if (!proctext::shouldAutoRelease(m_sys, availThreshold, m_lastAutoRelease, now,
                                     kAutoCooldownSecs))
        return;

    const QVector<quint32> pids = proctext::pickReleaseCandidates(
        m_processes, kAutoMinBytes, kAutoMaxCount, QCoreApplication::applicationPid());
    if (pids.isEmpty())
        return;

    m_lastAutoRelease = now;   // 先记账再动手：失败了也要等冷却，避免每 2 秒重试一次
    releaseProcesses(pids, true);
}

void ProcMonPanel::updateSelectionState()
{
    m_releaseSelected->setEnabled(selectedPid() != 0 && procprobe::canTrimOtherProcesses());
}

quint32 ProcMonPanel::selectedPid() const
{
    QTreeWidgetItem *item = m_table->currentItem();
    if (!item || !item->isSelected())
        return 0;
    return item->data(ColName, Qt::UserRole).toUInt();
}

void ProcMonPanel::applySortColumn(int column)
{
    proctext::SortKey key = proctext::SortKey::Memory;
    switch (column) {
    case ColName:   key = proctext::SortKey::Name; break;
    case ColPid:    key = proctext::SortKey::Pid; break;
    case ColCpu:    key = proctext::SortKey::Cpu; break;
    case ColMemory: key = proctext::SortKey::Memory; break;
    case ColDisk:   key = proctext::SortKey::Disk; break;
    case ColPort:   key = proctext::SortKey::Port; break;
    default:        return;   // 网络列没有数字可排，点了不响应
    }

    // 点同一列翻转方向，换列则回到该列最自然的顺序：
    // 名称/PID/端口从小到大，指标列从大到小（用户总是先看最吃资源的那几个）
    if (key == m_sortKey) {
        m_sortDescending = !m_sortDescending;
    } else {
        m_sortKey = key;
        m_sortDescending = key == proctext::SortKey::Cpu
            || key == proctext::SortKey::Memory || key == proctext::SortKey::Disk;
    }
    m_table->header()->setSortIndicator(column,
                                        m_sortDescending ? Qt::DescendingOrder
                                                         : Qt::AscendingOrder);
    rebuildTable();
}

void ProcMonPanel::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_table->itemAt(pos);
    if (!item)
        return;

    const quint32 pid = item->data(ColName, Qt::UserRole).toUInt();
    const QString name = item->text(ColName);
    const bool isSelf = pid == QCoreApplication::applicationPid();

    QString exePath;
    for (const procprobe::ProcessInfo &process : m_processes) {
        if (process.pid == pid) {
            exePath = process.exePath;
            break;
        }
    }

    QMenu menu(this);
    QAction *release = menu.addAction(QStringLiteral("释放「%1」的内存").arg(name));
    release->setEnabled(procprobe::canTrimOtherProcesses());
    menu.addSeparator();
    QAction *restart = menu.addAction(QStringLiteral("重启「%1」").arg(name));
    restart->setEnabled(!isSelf);
    QAction *kill = menu.addAction(QStringLiteral("结束「%1」").arg(name));
    kill->setEnabled(!isSelf);
    menu.addSeparator();
    QAction *reveal = menu.addAction(QStringLiteral("打开文件所在位置"));
    reveal->setEnabled(!exePath.isEmpty());

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == release)
        releaseProcesses(QVector<quint32>{pid}, false);
    else if (chosen == restart)
        restartProcess(pid, name);
    else if (chosen == kill)
        killProcessWithConfirm(pid, name);
    else if (chosen == reveal)
        revealProcess(pid, name);
}

bool ProcMonPanel::killProcessWithConfirm(quint32 pid, const QString &name)
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("结束进程"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("结束「%1」（PID %2）？").arg(name).arg(pid));
    box.setInformativeText(QStringLiteral("未保存的数据会丢失。"));
    // 不用 setStandardButtons + setButtonText：后者在 Qt 6 已标记过时，改用 addButton
    QPushButton *confirm = box.addButton(QStringLiteral("结束"), QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != confirm)
        return false;

    const procprobe::KillOutcome outcome = procprobe::killProcess(pid);
    refresh();
    if (outcome.ok)
        setStatus(QStringLiteral("已结束「%1」").arg(name), false, 5);
    else
        setStatus(QStringLiteral("结束失败：%1").arg(outcome.error), true, 8);
    return outcome.ok;
}

void ProcMonPanel::restartProcess(quint32 pid, const QString &name)
{
    // 先把命令行读出来再动手：杀掉之后进程信息就没了，来不及读
    const procprobe::ProcessCommand command = procprobe::readCommand(pid);
    if (!command.valid) {
        setStatus(QStringLiteral("无法重启「%1」：%2").arg(name, command.error), true, 8);
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("重启进程"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("重启「%1」（PID %2）？").arg(name).arg(pid));
    box.setInformativeText(QStringLiteral("会先结束该进程，再用同样的命令行重新启动。"
                                          "未保存的数据会丢失。"));
    QPushButton *confirm = box.addButton(QStringLiteral("重启"), QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != confirm)
        return;

    const procprobe::KillOutcome outcome = procprobe::killProcess(pid);
    if (!outcome.ok) {
        refresh();
        setStatus(QStringLiteral("重启失败（结束阶段）：%1").arg(outcome.error), true, 8);
        return;
    }

    // 结束完成后立刻拉起来：killProcess 已经等进程退出，这里不必再 sleep
    const bool started = QProcess::startDetached(command.program, command.args,
                                                 command.workingDir);
    refresh();
    if (started) {
        setStatus(QStringLiteral("已重启「%1」").arg(QFileInfo(command.program).fileName()),
                  false, 6);
    } else {
        setStatus(QStringLiteral("已结束，但重新启动失败：%1").arg(command.program), true, 10);
    }
}

void ProcMonPanel::revealProcess(quint32 pid, const QString &name)
{
    QString exePath;
    for (const procprobe::ProcessInfo &process : m_processes) {
        if (process.pid == pid) {
            exePath = process.exePath;
            break;
        }
    }

    QString error;
    if (procprobe::revealProcessFile(exePath, &error))
        setStatus(QStringLiteral("已在文件管理器中定位「%1」").arg(name), false, 4);
    else
        setStatus(error, true, 8);
}

void ProcMonPanel::setStatus(const QString &text, bool warn, int holdSecs)
{
    m_status->setText(text);
    const QString state = warn ? QStringLiteral("warn") : QStringLiteral("normal");
    if (m_status->property("state").toString() != state) {
        m_status->setProperty("state", state);
        repolish(m_status);
    }
    m_statusHoldUntil = holdSecs > 0 ? QDateTime::currentDateTime().addSecs(holdSecs)
                                     : QDateTime();
}

#include "MemMonPanel.h"

#include "MemText.h"   // 单位换算 / 筛选排序 / 回收候选（纯逻辑，可单独测试）

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QAction>
#include <QEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QHideEvent>
#include <QShowEvent>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

/// 采样间隔。1 秒会把 CPU 用在"看数字跳动"上（每次要枚举几百个进程），
/// 3 秒又会让"刚打开一个大程序"的反馈太迟钝 —— 2 秒是折中。
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

/// 阈值下拉框右侧的小三角。
///
/// 为什么不交给 qss：给 `QComboBox::drop-down` 写规则能去掉原生那条甩到圆角框外的
/// 深色 L 形边框，但 Qt 同时就不画默认箭头了；改用 `::down-arrow { image }` 会把图片
/// 重复画到文字后面，改成 `background-image` 又整片平铺。自绘一个透明子控件最省事，
/// 位置由 MemMonPanel::eventFilter 跟着下拉框尺寸摆。
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
        painter.setBrush(isEnabled() ? QColor(0x94, 0xA3, 0xB8) : QColor(0xC0, 0xC9, 0xD6));
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

/// 概览卡等级 → qss 用的字符串（对应 #memPercent[level="..."] 与 #memBar[level="..."]）。
QString levelName(int level)
{
    if (level >= 2)
        return QStringLiteral("danger");
    if (level == 1)
        return QStringLiteral("warn");
    return QStringLiteral("normal");
}

} // namespace

MemMonPanel::MemMonPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐，视觉上像同一栏
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(12);

    // ---------------------------------------------------------------- 概览卡
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("memCard"));
    auto *cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(18, 12, 18, 12);
    cardLayout->setSpacing(18);

    auto *percentBox = new QVBoxLayout;
    percentBox->setSpacing(0);
    auto *caption = new QLabel(QStringLiteral("内存占用"), card);
    caption->setObjectName(QStringLiteral("memCaption"));
    m_percent = new QLabel(QStringLiteral("—"), card);
    m_percent->setObjectName(QStringLiteral("memPercent"));
    percentBox->addWidget(caption);
    percentBox->addWidget(m_percent);
    percentBox->addStretch(1);
    cardLayout->addLayout(percentBox);

    auto *barBox = new QVBoxLayout;
    barBox->setSpacing(8);
    m_used = new QLabel(QStringLiteral("正在读取…"), card);
    m_used->setObjectName(QStringLiteral("memUsed"));
    m_bar = new QProgressBar(card);
    m_bar->setObjectName(QStringLiteral("memBar"));
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
    m_avail->setObjectName(QStringLiteral("memAvail"));
    m_released = new QLabel(QStringLiteral("本次已释放 0 B"), card);
    m_released->setObjectName(QStringLiteral("memReleased"));
    statBox->addStretch(1);
    statBox->addWidget(m_avail, 0, Qt::AlignRight);
    statBox->addWidget(m_released, 0, Qt::AlignRight);
    statBox->addStretch(1);
    cardLayout->addLayout(statBox);

    layout->addWidget(card);

    // ---------------------------------------------------------------- 操作条
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("memSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索进程名或 PID"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(240);
    toolbar->addWidget(m_search, 1);

    m_auto = new QCheckBox(QStringLiteral("自动释放"), this);
    m_auto->setObjectName(QStringLiteral("memAuto"));
    m_auto->setToolTip(QStringLiteral("可用内存跌破设定阈值时，自动回收占用最大的几个进程"
                                      "（两次之间至少隔 %1 秒）").arg(kAutoCooldownSecs));
    toolbar->addWidget(m_auto);

    m_threshold = new QComboBox(this);
    m_threshold->setObjectName(QStringLiteral("memThreshold"));
    for (const int percent : {10, 15, 20, 25})
        m_threshold->addItem(QStringLiteral("可用 < %1%").arg(percent), percent);
    m_threshold->setCurrentIndex(1);   // 默认 15%
    m_threshold->setEnabled(false);
    toolbar->addWidget(m_threshold);
    m_comboArrow = new ComboArrow(m_threshold);
    m_comboArrow->setObjectName(QStringLiteral("memComboArrow"));
    m_threshold->installEventFilter(this);

    toolbar->addStretch(1);

    m_releaseSelected = new QPushButton(QStringLiteral("释放选中"), this);
    m_releaseSelected->setObjectName(QStringLiteral("memReleaseSelected"));
    m_releaseSelected->setEnabled(false);
    toolbar->addWidget(m_releaseSelected);

    m_releaseAll = new QPushButton(QStringLiteral("释放内存"), this);
    m_releaseAll->setObjectName(QStringLiteral("memReleaseAll"));
    m_releaseAll->setProperty("accent", true);   // 主操作
    m_releaseAll->setToolTip(QStringLiteral("把 %1 以上进程的工作集刷入页面文件；"
                                            "回收量显示在下方状态栏").arg(
        memtext::formatBytes(kManualMinBytes)));
    toolbar->addWidget(m_releaseAll);

    layout->addLayout(toolbar);

    // ---------------------------------------------------------------- 进程表
    m_table = new QTreeWidget(this);
    m_table->setObjectName(QStringLiteral("memTable"));
    m_table->setColumnCount(4);
    m_table->setHeaderLabels({QStringLiteral("进程"), QStringLiteral("PID"),
                              QStringLiteral("内存"), QStringLiteral("占比")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setItemDelegate(new RowHeightDelegate(m_table));

    QHeaderView *header = m_table->header();
    header->setFixedHeight(30);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    for (const int column : {1, 2, 3})
        header->setSectionResizeMode(column, QHeaderView::Fixed);
    header->resizeSection(1, 90);
    header->resizeSection(2, 110);
    header->resizeSection(3, 80);
    for (const int column : {1, 2, 3})
        m_table->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);

    layout->addWidget(m_table, 1);

    // ---------------------------------------------------------------- 底部
    auto *footer = new QHBoxLayout;
    footer->setSpacing(12);
    m_status = new QLabel(QStringLiteral("正在读取…"), this);
    m_status->setObjectName(QStringLiteral("memStatus"));
    auto *hint = new QLabel(QStringLiteral("右键进程可单独释放；释放会让相关进程短期内变慢"),
                            this);
    hint->setObjectName(QStringLiteral("memHint"));
    hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    footer->addWidget(m_status, 1);
    footer->addWidget(hint, 0);
    layout->addLayout(footer);

    // ---------------------------------------------------------------- 连接
    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &MemMonPanel::refresh);

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
        if (checked && !memprobe::canTrimOtherProcesses()) {
            m_auto->setChecked(false);
            setStatus(memprobe::unsupportedTrimReason(), true, 8);
            return;
        }
        m_threshold->setEnabled(checked);
        if (checked)
            refresh();   // 已经在低内存状态时立刻生效，不用等下一轮
    });

    connect(m_threshold, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (m_auto->isChecked())
            refresh();
    });

    connect(m_releaseAll, &QPushButton::clicked, this, &MemMonPanel::releaseAll);
    connect(m_releaseSelected, &QPushButton::clicked, this, &MemMonPanel::releaseSelected);
    connect(m_table, &QTreeWidget::itemSelectionChanged, this, &MemMonPanel::updateSelectionState);
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, &MemMonPanel::showContextMenu);

    updateSummary();
    updateSelectionState();
}

void MemMonPanel::showEvent(QShowEvent *event)
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

void MemMonPanel::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    QWidget::hideEvent(event);
}

bool MemMonPanel::eventFilter(QObject *watched, QEvent *event)
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

void MemMonPanel::refresh()
{
    const memprobe::SystemMemory system = memprobe::readSystemMemory();
    if (system.valid)
        m_sys = system;
    m_processes = memprobe::listProcesses();

    updateSummary();
    rebuildTable();
    maybeAutoRelease();

    // 刚显示过释放结果时别盖掉它（用户正看着"回收了 1.2 GB"）
    if (m_statusHoldUntil.isValid() && QDateTime::currentDateTime() < m_statusHoldUntil)
        return;

    const int shown = m_table->topLevelItemCount();
    QString text = QStringLiteral("共 %1 个进程").arg(m_processes.size());
    if (shown != m_processes.size())
        text += QStringLiteral("，匹配 %1 个").arg(shown);
    text += QStringLiteral(" · 每 %1 秒刷新").arg(kRefreshIntervalMs / 1000);
    setStatus(text, false, 0);
}

void MemMonPanel::updateSummary()
{
    if (!m_sys.valid || m_sys.totalBytes == 0) {
        m_percent->setText(QStringLiteral("—"));
        m_used->setText(QStringLiteral("读不到本机内存信息"));
        m_avail->setText(QStringLiteral("可用 —"));
        m_bar->setValue(0);
        return;
    }

    const quint64 avail = qMin(m_sys.availBytes, m_sys.totalBytes);
    const double used = memtext::usedPercent(m_sys);

    m_percent->setText(memtext::formatPercent(used));
    m_used->setText(QStringLiteral("已用 %1 / 共 %2")
                        .arg(memtext::formatBytes(m_sys.totalBytes - avail),
                             memtext::formatBytes(m_sys.totalBytes)));
    m_avail->setText(QStringLiteral("可用 %1").arg(memtext::formatBytes(avail)));
    m_released->setText(QStringLiteral("本次已释放 %1").arg(memtext::formatBytes(m_releasedTotal)));
    m_bar->setValue(qRound(used));

    const int level = memtext::levelFor(used);
    if (level == m_lastLevel)
        return;
    m_lastLevel = level;
    const QString name = levelName(level);
    for (QWidget *widget : {static_cast<QWidget *>(m_percent), static_cast<QWidget *>(m_bar)}) {
        widget->setProperty("level", name);
        repolish(widget);
    }
}

void MemMonPanel::rebuildTable()
{
    const QVector<memprobe::ProcessMemory> rows =
        memtext::filterAndSort(m_processes, m_search->text().trimmed());

    QVector<quint32> pids;
    pids.reserve(rows.size());
    for (const memprobe::ProcessMemory &process : rows)
        pids.append(process.pid);

    // 顺序没变就只改文字：整表重建每 2 秒闪一下，而且会打断鼠标悬停/拖选
    if (pids == m_rowPids && m_table->topLevelItemCount() == rows.size()) {
        for (int i = 0; i < rows.size(); ++i)
            updateRow(m_table->topLevelItem(i), rows.at(i));
    } else {
        rebuildTableFully(rows);
        m_rowPids = pids;
    }

    updateSelectionState();
}

void MemMonPanel::rebuildTableFully(const QVector<memprobe::ProcessMemory> &rows)
{
    const quint32 keepPid = selectedPid();
    const int scroll = m_table->verticalScrollBar()->value();

    m_table->setUpdatesEnabled(false);
    m_table->clear();
    QTreeWidgetItem *restore = nullptr;
    for (const memprobe::ProcessMemory &process : rows) {
        auto *item = new QTreeWidgetItem(m_table);
        item->setData(0, Qt::UserRole, process.pid);
        for (const int column : {1, 2, 3})
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

void MemMonPanel::updateRow(QTreeWidgetItem *item, const memprobe::ProcessMemory &process)
{
    const QString name = process.name;
    if (item->text(0) != name)
        item->setText(0, name);

    const QString pid = QString::number(process.pid);
    if (item->text(1) != pid)
        item->setText(1, pid);

    const QString memory = process.readable ? memtext::formatBytes(process.workingSet)
                                            : QStringLiteral("—");
    if (item->text(2) != memory)
        item->setText(2, memory);

    const QString percent = process.readable
        ? rowPercentText(process.workingSet, m_sys.totalBytes)
        : QStringLiteral("—");
    if (item->text(3) != percent)
        item->setText(3, percent);

    const QString tip = process.readable
        ? QString()
        : QStringLiteral("受保护的系统进程，读不到内存信息");
    if (item->toolTip(0) != tip)
        item->setToolTip(0, tip);

    if (item->data(0, Qt::UserRole).toUInt() != process.pid)
        item->setData(0, Qt::UserRole, process.pid);
}

void MemMonPanel::releaseAll()
{
    // 自己（面板所在进程）不参与：刷自己没收益，还会让刚画好的界面重新缺页
    const QVector<quint32> pids = memtext::pickReleaseCandidates(
        m_processes, kManualMinBytes, kManualMaxCount, QCoreApplication::applicationPid());
    releaseProcesses(pids, false);
}

void MemMonPanel::releaseSelected()
{
    const quint32 pid = selectedPid();
    if (pid == 0)
        return;
    // 用户明确点了这一行就不再卡 64 MB 门槛：小进程也可以手动刷
    releaseProcesses(QVector<quint32>{pid}, false);
}

void MemMonPanel::releaseProcesses(const QVector<quint32> &pids, bool automatic)
{
    if (!memprobe::canTrimOtherProcesses()) {
        setStatus(memprobe::unsupportedTrimReason(), true, 8);
        return;
    }
    if (pids.isEmpty()) {
        setStatus(QStringLiteral("没有可释放的进程（都小于 %1）")
                      .arg(memtext::formatBytes(kManualMinBytes)),
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
        const memprobe::TrimOutcome outcome = memprobe::trimProcess(pid);
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
                    .arg(memtext::formatBytes(freed))
                    .arg(succeeded);
        if (denied > 0)
            text += QStringLiteral("，%1 个无权限").arg(denied);
        setStatus(text, false, 6);
    }
}

void MemMonPanel::maybeAutoRelease()
{
    if (m_releasing || !m_auto->isChecked() || !m_sys.valid)
        return;

    const QDateTime now = QDateTime::currentDateTime();
    const int threshold = m_threshold->currentData().toInt();
    if (!memtext::shouldAutoRelease(m_sys, threshold, m_lastAutoRelease, now, kAutoCooldownSecs))
        return;

    const QVector<quint32> pids = memtext::pickReleaseCandidates(
        m_processes, kAutoMinBytes, kAutoMaxCount, QCoreApplication::applicationPid());
    if (pids.isEmpty())
        return;

    m_lastAutoRelease = now;   // 先记账再动手：失败了也要等冷却，避免每 2 秒重试一次
    releaseProcesses(pids, true);
}

void MemMonPanel::updateSelectionState()
{
    m_releaseSelected->setEnabled(selectedPid() != 0);
}

quint32 MemMonPanel::selectedPid() const
{
    QTreeWidgetItem *item = m_table->currentItem();
    if (!item || !item->isSelected())
        return 0;
    return item->data(0, Qt::UserRole).toUInt();
}

void MemMonPanel::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_table->itemAt(pos);
    if (!item)
        return;

    const quint32 pid = item->data(0, Qt::UserRole).toUInt();
    const QString name = item->text(0);

    QMenu menu(this);
    QAction *release = menu.addAction(QStringLiteral("释放「%1」的内存").arg(name));
    release->setEnabled(memprobe::canTrimOtherProcesses());
    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (chosen == release)
        releaseProcesses(QVector<quint32>{pid}, false);
}

void MemMonPanel::setStatus(const QString &text, bool warn, int holdSecs)
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

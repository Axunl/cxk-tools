#include "FileLockPanel.h"

#include "FileLockProbe.h"

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSharedPointer>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

/// 改动态属性后必须重新 polish，否则 qss 的 [state="..."] / [hovered="true"]
/// 不会立刻生效（与 MemMonPanel 同一套做法）。
void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

/// 拖拽区左侧的「向上箭头 + 方框」图标。
/// 自绘而不用字体符号：不同平台的 emoji / 符号字体差别很大，画一遍最稳。
class UploadGlyph : public QWidget
{
public:
    explicit UploadGlyph(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(36, 36);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor blue(0x4F, 0x6B, 0xF6);

        QPainterPath box;
        box.addRoundedRect(QRectF(1.5, 1.5, 33, 33), 8, 8);
        painter.setPen(QPen(blue, 1.8));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(box);

        // 箭头：竖线 + 两侧斜线，居中摆在方框里
        painter.setPen(QPen(blue, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(18, 25, 18, 12);
        QPainterPath head;
        head.moveTo(12, 17);
        head.lineTo(18, 11);
        head.lineTo(24, 17);
        painter.drawPath(head);
    }
};

/// 虚线拖拽区。刻意不带 Q_OBJECT：只需要覆盖几个事件 + 一个回调，
/// 放进 .cpp 的匿名命名空间就不必多一份 moc 产物。
class DropZone : public QFrame
{
public:
    explicit DropZone(QWidget *parent, std::function<void(const QStringList &)> onDrop)
        : QFrame(parent), m_onDrop(std::move(onDrop))
    {
        setObjectName(QStringLiteral("flDropZone"));
        // QWidget 子类默认不画样式背景，虚线框和 hover 底色都依赖这个属性
        setAttribute(Qt::WA_StyledBackground, true);
        setAcceptDrops(true);
        setMinimumHeight(100);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(18, 12, 18, 12);
        layout->setSpacing(14);
        layout->addStretch(1);
        layout->addWidget(new UploadGlyph(this));

        auto *texts = new QVBoxLayout;
        texts->setSpacing(4);
        auto *title = new QLabel(QStringLiteral("拖拽文件或文件夹到这里"), this);
        title->setObjectName(QStringLiteral("flDropTitle"));
        auto *hint = new QLabel(
            QStringLiteral("支持拖拽多个文件或整个文件夹，是否递归由上方选项控制"), this);
        hint->setObjectName(QStringLiteral("flDropHint"));
        hint->setWordWrap(true);
        texts->addWidget(title);
        texts->addWidget(hint);
        layout->addLayout(texts, 5);
        layout->addStretch(1);
    }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData() && event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
            setHovered(true);
        } else {
            QFrame::dragEnterEvent(event);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData() && event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            QFrame::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setHovered(false);
        QFrame::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        setHovered(false);
        if (!event->mimeData() || !event->mimeData()->hasUrls())
            return;
        QStringList paths;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile())
                paths.append(url.toLocalFile());
        }
        if (paths.isEmpty())
            return;
        event->acceptProposedAction();
        m_onDrop(paths);
    }

private:
    void setHovered(bool on)
    {
        if (m_hovered == on)
            return;
        m_hovered = on;
        setProperty("hovered", on ? QStringLiteral("true") : QStringLiteral("false"));
        repolish(this);
    }

    std::function<void(const QStringList &)> m_onDrop;
    bool m_hovered = false;
};

} // namespace

FileLockPanel::FileLockPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐（与其他插件面板一致）
    root->setContentsMargins(28, 0, 28, 20);
    root->setSpacing(12);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("flCard"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 18, 20, 18);
    cardLayout->setSpacing(14);

    // ------------------------------------------------------------ 路径行
    auto *pathRow = new QHBoxLayout;
    pathRow->setSpacing(10);
    auto *pathLabel = new QLabel(QStringLiteral("路径"), card);
    pathLabel->setObjectName(QStringLiteral("flPathLabel"));
    pathRow->addWidget(pathLabel);

    m_path = new QLineEdit(card);
    m_path->setObjectName(QStringLiteral("flPath"));
    m_path->setPlaceholderText(
        QStringLiteral("输入文件或文件夹路径，或直接拖拽到下方区域"));
    m_path->setToolTip(QStringLiteral("多个路径用分号分隔；文件夹是否包含子目录由「递归查询」决定"));
    pathRow->addWidget(m_path, 1);

    m_browse = new QPushButton(QStringLiteral("浏览..."), card);
    m_browse->setObjectName(QStringLiteral("flBrowse"));
    pathRow->addWidget(m_browse);

    m_recursive = new QCheckBox(QStringLiteral("递归查询"), card);
    m_recursive->setObjectName(QStringLiteral("flRecursive"));
    m_recursive->setChecked(true);
    m_recursive->setToolTip(QStringLiteral("勾选后，拖入文件夹时连同子目录里的文件一起检测"));
    pathRow->addWidget(m_recursive);
    cardLayout->addLayout(pathRow);

    // ------------------------------------------------------------ 拖拽区
    cardLayout->addWidget(new DropZone(card, [this](const QStringList &paths) {
        appendDroppedPaths(paths);
    }));

    // ------------------------------------------------------------ 操作行
    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    m_detect = new QPushButton(QStringLiteral("检测占用"), card);
    m_detect->setObjectName(QStringLiteral("flDetect"));
    m_detect->setProperty("accent", true);
    actions->addWidget(m_detect, 1);

    m_release = new QPushButton(QStringLiteral("解除占用"), card);
    m_release->setObjectName(QStringLiteral("flRelease"));
    actions->addWidget(m_release, 1);

    m_terminate = new QPushButton(QStringLiteral("强制结束"), card);
    m_terminate->setObjectName(QStringLiteral("flTerminate"));
    actions->addWidget(m_terminate, 1);
    cardLayout->addLayout(actions);

    // ------------------------------------------------------------ 结果表
    m_stack = new QStackedWidget(card);
    m_empty = new QLabel(QStringLiteral("还没有检测结果\n\n选择或拖入文件 / 文件夹后，点「检测占用」"),
                         m_stack);
    m_empty->setObjectName(QStringLiteral("flEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_stack->addWidget(m_empty);

    m_table = new QTableWidget(m_stack);
    m_table->setObjectName(QStringLiteral("flTable"));
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({QStringLiteral("程序"), QStringLiteral("PID"),
                                        QStringLiteral("类型"), QStringLiteral("状态"),
                                        QStringLiteral("操作")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setShowGrid(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->setMinimumHeight(160);

    QHeaderView *header = m_table->horizontalHeader();
    header->setFixedHeight(34);
    header->setStretchLastSection(false);
    // 第一列带复选框 + 进程名，标题若按默认居中会和行内容明显错开
    m_table->horizontalHeaderItem(0)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column)
        header->setSectionResizeMode(column, QHeaderView::Fixed);
    header->resizeSection(1, 90);
    header->resizeSection(2, 120);
    header->resizeSection(3, 100);
    header->resizeSection(4, 90);
    m_stack->addWidget(m_table);
    cardLayout->addWidget(m_stack, 1);

    root->addWidget(card, 1);

    // ------------------------------------------------------------ 状态栏
    auto *footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("flFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 8, 12, 8);
    footerLayout->setSpacing(10);

    m_dot = new QLabel(footer);
    m_dot->setObjectName(QStringLiteral("flStatusDot"));
    m_dot->setFixedSize(10, 10);
    m_dot->setAttribute(Qt::WA_StyledBackground, true);
    footerLayout->addWidget(m_dot);

    m_status = new QLabel(QStringLiteral("准备就绪"), footer);
    m_status->setObjectName(QStringLiteral("flStatus"));
    footerLayout->addWidget(m_status, 1);

    auto *divider = new QLabel(QStringLiteral("|"), footer);
    divider->setObjectName(QStringLiteral("flDivider"));
    footerLayout->addWidget(divider);

    m_selected = new QLabel(QStringLiteral("已选择 0 个程序"), footer);
    m_selected->setObjectName(QStringLiteral("flSelected"));
    footerLayout->addWidget(m_selected);
    footerLayout->addStretch(1);

    m_clear = new QPushButton(QStringLiteral("清空"), footer);
    m_clear->setObjectName(QStringLiteral("flClear"));
    footerLayout->addWidget(m_clear);
    root->addWidget(footer, 0);

    // ------------------------------------------------------------ 连接
    connect(m_browse, &QPushButton::clicked, this, &FileLockPanel::browsePath);
    connect(m_detect, &QPushButton::clicked, this, &FileLockPanel::startScan);
    connect(m_release, &QPushButton::clicked, this, &FileLockPanel::releaseSelected);
    connect(m_terminate, &QPushButton::clicked, this, &FileLockPanel::terminateSelected);
    connect(m_clear, &QPushButton::clicked, this, &FileLockPanel::clearAll);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (!m_building && item && item->column() == 0)
            updateSelectionState();
    });

    rebuildTable();
    updateSelectionState();
    setStatus(QStringLiteral("准备就绪"), false, false);
}

FileLockPanel::~FileLockPanel()
{
    // 宿主可能在检测途中销毁面板（回首页 / 关闭独立窗口）。worker 是纯计算，
    // quit() 中断不了它，只能等它自然跑完；结果回调以 this 为 context，
    // 对象销毁后 Qt 会丢弃那次投递，不会访问已释放的面板。
    if (m_scanThread && m_scanThread->isRunning())
        m_scanThread->wait();
}

void FileLockPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 延后一拍：同步设焦点会被宿主的焦点链覆盖（同 JsonFmtPanel）
    QTimer::singleShot(0, this, [this] { m_path->setFocus(); });
}

void FileLockPanel::browsePath()
{
    QMenu menu(this);
    QAction *fileAction = menu.addAction(QStringLiteral("选择文件…"));
    QAction *dirAction = menu.addAction(QStringLiteral("选择文件夹…"));
    QAction *chosen = menu.exec(m_browse->mapToGlobal(QPoint(0, m_browse->height() + 4)));

    QString path;
    if (chosen == fileAction) {
        path = QFileDialog::getOpenFileName(this, QStringLiteral("选择文件"),
                                            QDir::homePath());
    } else if (chosen == dirAction) {
        path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择文件夹"),
                                                 QDir::homePath());
    }
    if (!path.isEmpty())
        appendDroppedPaths(QStringList{path});
}

void FileLockPanel::appendDroppedPaths(const QStringList &paths)
{
    QStringList current = filelock::parsePathText(m_path->text());
    int added = 0;
    for (const QString &path : paths) {
        if (current.contains(path))
            continue;
        current.append(path);
        ++added;
    }
    // 用分号串起来而不是只留第一个：输入框能直接看见全部目标，也能手改
    m_path->setText(current.join(QStringLiteral("; ")));
    if (added > 0) {
        setStatus(QStringLiteral("已添加 %1 个目标，点「检测占用」开始").arg(added),
                  false, false);
    }
}

void FileLockPanel::startScan()
{
    if (m_scanning)
        return;
    if (!fileprobe::canDetect()) {
        setStatus(fileprobe::unsupportedDetectReason(), true, false);
        return;
    }
    const QStringList inputs = filelock::parsePathText(m_path->text());
    if (inputs.isEmpty()) {
        setStatus(QStringLiteral("请先输入或拖入文件 / 文件夹路径"), true, false);
        return;
    }

    const bool recursive = m_recursive->isChecked();
    const quint64 ownPid = selfPid();
    m_scanning = true;
    updateSelectionState();
    setStatus(QStringLiteral("正在检测占用…（%1 个目标）").arg(inputs.size()),
              false, true);

    // 结果放共享指针：worker 只写数据，不碰任何 QWidget；finished 的两个
    // 连接一个负责回收线程对象、一个回到 UI 线程刷新表格。
    auto result = QSharedPointer<filelock::ScanOutcome>::create();
    QThread *thread = QThread::create([inputs, recursive, ownPid, result]() {
        *result = fileprobe::scan(inputs, recursive, ownPid);
    });
    m_scanThread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, result]() {
        m_scanThread = nullptr;
        m_scanning = false;
        applyScanResult(*result);
    });
    thread->start();
}

void FileLockPanel::applyScanResult(const filelock::ScanOutcome &outcome)
{
    m_holders = outcome.holders;
    rebuildTable();
    updateSelectionState();

    if (!outcome.error.isEmpty()) {
        setStatus(outcome.error, true, false);
        return;
    }
    QString text = m_holders.isEmpty()
                       ? QStringLiteral("检测完成：没有程序占用这些文件")
                       : QStringLiteral("检测完成：找到 %1 个程序（检查了 %2 个文件）")
                             .arg(m_holders.size())
                             .arg(outcome.scannedFiles);
    if (outcome.truncated) {
        text += QStringLiteral("；文件数超过 %1 个，只检查了前 %1 个，请缩小范围")
                    .arg(filelock::kMaxExpandedFiles);
    }
    setStatus(text, false, false);
}

void FileLockPanel::rebuildTable()
{
    m_building = true;
    m_table->clearContents();
    m_table->setRowCount(m_holders.size());

    for (int row = 0; row < m_holders.size(); ++row) {
        const filelock::Holder &holder = m_holders.at(row);

        auto *name = new QTableWidgetItem(holder.name);
        name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        name->setCheckState(Qt::Unchecked);
        name->setData(Qt::UserRole, QVariant::fromValue(holder.pid));
        name->setToolTip(holder.hitPath.isEmpty()
                             ? QStringLiteral("PID %1").arg(holder.pid)
                             : QStringLiteral("%1\n命中：%2")
                                   .arg(QString::number(holder.pid), holder.hitPath));
        m_table->setItem(row, 0, name);

        auto *pid = new QTableWidgetItem(QString::number(holder.pid));
        pid->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 1, pid);

        auto *type = new QTableWidgetItem(holder.type);
        type->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 2, type);

        auto *state = new QTableWidgetItem(holder.status);
        state->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 3, state);

        auto *action = new QPushButton(QStringLiteral("结束"), m_table);
        action->setObjectName(QStringLiteral("flRowAction"));
        action->setToolTip(QStringLiteral("强制结束这个进程"));
        action->setEnabled(fileprobe::canTerminate());
        const quint64 targetPid = holder.pid;
        connect(action, &QPushButton::clicked, this, [this, targetPid] {
            terminateOne(targetPid);
        });
        m_table->setCellWidget(row, 4, action);
    }
    m_building = false;

    // 不能用三元表达式选页面：QLabel* 与 QTableWidget* 之间没有隐式转换，
    // MSVC 会报 C2446；老老实实用分支。
    if (m_holders.isEmpty())
        m_stack->setCurrentWidget(m_empty);
    else
        m_stack->setCurrentWidget(m_table);
}

void FileLockPanel::updateSelectionState()
{
    const int count = selectedPids().size();
    m_selected->setText(QStringLiteral("已选择 %1 个程序").arg(count));

    m_detect->setEnabled(!m_scanning && fileprobe::canDetect());
    m_release->setEnabled(!m_scanning && count > 0 && fileprobe::canReleaseHandles());
    m_terminate->setEnabled(!m_scanning && count > 0 && fileprobe::canTerminate());

    // 平台不支持时按钮是灰的，必须说清原因，不留一个「看着能点其实没用」的控件
    m_release->setToolTip(fileprobe::canReleaseHandles()
                              ? QStringLiteral("关闭所选进程里指向目标文件的句柄，进程本身不退出")
                              : fileprobe::unsupportedReleaseReason());
    m_terminate->setToolTip(fileprobe::canTerminate()
                                ? QStringLiteral("强制结束所选进程，未保存的数据会丢失")
                                : fileprobe::unsupportedTerminateReason());
}

QVector<quint64> FileLockPanel::selectedPids() const
{
    QVector<quint64> pids;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *item = m_table->item(row, 0);
        if (item && item->checkState() == Qt::Checked)
            pids.append(item->data(Qt::UserRole).toULongLong());
    }
    return pids;
}

void FileLockPanel::releaseSelected()
{
    const QVector<quint64> pids = selectedPids();
    if (pids.isEmpty())
        return;
    if (!fileprobe::canReleaseHandles()) {
        setStatus(fileprobe::unsupportedReleaseReason(), true, false);
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("解除占用"));
    box.setText(QStringLiteral("将关闭所选 %1 个程序持有的目标文件句柄。").arg(pids.size()));
    box.setInformativeText(
        QStringLiteral("这些程序不会退出，但下次读写该文件时可能报错、崩溃或丢失未保存的数据。建议先保存工作。"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    box.button(QMessageBox::Yes)->setText(QStringLiteral("继续解除"));
    box.button(QMessageBox::No)->setText(QStringLiteral("取消"));
    if (box.exec() != QMessageBox::Yes)
        return;

    const QStringList inputs = filelock::parsePathText(m_path->text());
    const filelock::ActionOutcome outcome =
        fileprobe::release(inputs, m_recursive->isChecked(), pids, selfPid());
    markRows(outcome.succeededPids, QStringLiteral("已解除"));
    updateSelectionState();

    if (outcome.succeeded() == 0) {
        setStatus(outcome.error.isEmpty() ? QStringLiteral("没有句柄被关闭")
                                          : outcome.error,
                  true, false);
        return;
    }
    QString text = QStringLiteral("已关闭 %1 个文件句柄，涉及 %2 个程序；"
                                  "可再点「检测占用」确认")
                       .arg(outcome.closedHandles)
                       .arg(outcome.succeeded());
    if (outcome.failed > 0)
        text += QStringLiteral("，%1 个无权限").arg(outcome.failed);
    setStatus(text, false, false);
}

void FileLockPanel::terminateSelected()
{
    terminatePids(selectedPids());
}

void FileLockPanel::terminateOne(quint64 pid)
{
    if (pid == 0)
        return;
    terminatePids(QVector<quint64>{pid});
}

void FileLockPanel::terminatePids(const QVector<quint64> &pids)
{
    if (pids.isEmpty())
        return;
    if (!fileprobe::canTerminate()) {
        setStatus(fileprobe::unsupportedTerminateReason(), true, false);
        return;
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("强制结束进程"));
    box.setText(QStringLiteral("将强制结束所选 %1 个程序。").arg(pids.size()));
    box.setInformativeText(
        QStringLiteral("被结束的进程不会收到保存提示，未保存的数据会丢失。"
                       "系统关键进程与工具自身会被自动跳过。"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    box.button(QMessageBox::Yes)->setText(QStringLiteral("仍要结束"));
    box.button(QMessageBox::No)->setText(QStringLiteral("取消"));
    if (box.exec() != QMessageBox::Yes)
        return;

    const filelock::ActionOutcome outcome = fileprobe::terminate(pids, selfPid());
    markRows(outcome.succeededPids, QStringLiteral("已结束"));
    updateSelectionState();

    if (outcome.succeeded() == 0) {
        setStatus(outcome.error.isEmpty() ? QStringLiteral("没有进程被结束")
                                          : outcome.error,
                  true, false);
        return;
    }
    QString text = QStringLiteral("已向 %1 个程序发出结束指令").arg(outcome.succeeded());
    if (outcome.failed > 0)
        text += QStringLiteral("，%1 个失败（%2）").arg(outcome.failed).arg(outcome.error);
    setStatus(text, false, false);
}

void FileLockPanel::markRows(const QVector<quint64> &pids, const QString &status)
{
    if (pids.isEmpty())
        return;
    m_building = true;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem *name = m_table->item(row, 0);
        if (!name || !pids.contains(name->data(Qt::UserRole).toULongLong()))
            continue;
        name->setCheckState(Qt::Unchecked);
        if (QTableWidgetItem *state = m_table->item(row, 3))
            state->setText(status);
    }
    m_building = false;
}

void FileLockPanel::clearAll()
{
    m_path->clear();
    m_holders.clear();
    rebuildTable();
    updateSelectionState();
    setStatus(QStringLiteral("准备就绪"), false, false);
}

void FileLockPanel::setStatus(const QString &text, bool warn, bool busy)
{
    m_status->setText(text);
    const QString state = warn ? QStringLiteral("warn")
                               : (busy ? QStringLiteral("busy") : QStringLiteral("ready"));
    for (QWidget *widget : {static_cast<QWidget *>(m_status), static_cast<QWidget *>(m_dot)}) {
        if (widget->property("state").toString() == state)
            continue;
        widget->setProperty("state", state);
        repolish(widget);
    }
}

quint64 FileLockPanel::selfPid() const
{
    return static_cast<quint64>(QCoreApplication::applicationPid());
}

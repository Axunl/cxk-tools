#include "DiskScanPanel.h"

#include "LayoutMetrics.h"
#include "ThemePalette.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

/// 表格列号。散在代码里写 0/1/2/3 太容易错位，统一在这里对一次。
enum Column
{
    ColName = 0,
    ColSize,
    ColShare,
    ColFiles,
    ColHint,
    ColLevel
};

/// 行数据里的自定义角色。
/// 为什么不靠"显示文字"反查：大小列显示的是 "1.2 GB"，按文字排会排成字典序；
/// 路径与等级也必须原样带在行上，否则双击时又要回 m_items 里线性找一遍。
constexpr int kPathRole = Qt::UserRole;        ///< QString 绝对路径
constexpr int kIsDirRole = Qt::UserRole + 1;   ///< bool
constexpr int kShareRole = Qt::UserRole + 2;   ///< double，占比（0~1），给比例条 delegate 用
constexpr int kLevelRole = Qt::UserRole + 3;   ///< int（diskscan::Level），给徽章 delegate 用

/// 扫描中表格刷新的节流间隔。每算完一个子项就整表重建，遇到子项多的目录
/// （例如 C:\Windows\WinSxS）会一直闪；120ms 一次既跟得上又不打断浏览。
constexpr int kFlushIntervalMs = 120;

/// 「隐藏小于 50 MB」的阈值。低于这个量的项在整盘扫描里成千上万，
/// 全列出来只会把真正的大户挤出屏幕，而 50 MB 以下的目录清理收益基本为 0。
constexpr quint64 kSmallItemBytes = 50ull * 1024 * 1024;

/// 超过这个体积就提示"可能超出回收站容量"。回收站是按卷分配配额的，
/// 大目录丢进去时 Windows 会跳过回收站直接删 —— 这条提示是为了避免用户
/// 以为"反正能还原"。
constexpr quint64 kRecycleCapacityWarnBytes = 20ull * 1024 * 1024 * 1024;

/// 改动态属性后必须重新 polish，否则 qss 里 [level="warn"] 这类规则不会立刻生效。
void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

/// 下拉框右侧的小三角。
/// 全局 qss 为了消掉原生 ::drop-down 甩到圆角框外的边框写了规则，Qt 因此不再画
/// 默认箭头；自绘一个透明子控件最省事，位置由 eventFilter 跟着下拉框尺寸摆。
/// （与 ProcMonPanel / TranslatePanel 同一套做法，见 docs/PLUGIN.md）
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
        // 跟主题走：写死浅灰在深色底上几乎看不见
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
/// 决定，这里补一个最小高度，避免默认 ~20px 的行挤成一团。
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

/// 「占比」列：一条按比例填充的细条 + 右侧百分比文字。
///
/// 为什么值得自绘：只写 "23.4%" 需要用户在心里把这列数字横向比一遍；
/// 一条长度不一的条能让人一眼看出"这块最大"。颜色全部走主题令牌，
/// 深色模式下不用改一行代码。
class ShareBarDelegate : public QStyledItemDelegate
{
public:
    explicit ShareBarDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();   // 文字自己画，避免和比例条叠在一起

        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const double fraction = index.data(kShareRole).toDouble();
        const QRect area = option.rect.adjusted(10, 0, -10, 0);
        const int textWidth = 44;
        const QRect bar(area.left(), area.center().y() - 3,
                        qMax(6, area.width() - textWidth - 10), 6);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(xtools::theme::color(xtools::theme::Color::BgTrack));
        painter->drawRoundedRect(bar, 3, 3);

        const double clamped = qBound(0.0, fraction, 1.0);
        if (clamped > 0.0) {
            QRect filled = bar;
            filled.setWidth(qMax(3, qRound(bar.width() * clamped)));
            painter->setBrush(xtools::theme::color(xtools::theme::Color::Accent));
            painter->drawRoundedRect(filled, 3, 3);
        }

        const bool selected = option.state & QStyle::State_Selected;
        painter->setPen(xtools::theme::color(selected ? xtools::theme::Color::AccentText
                                                      : xtools::theme::Color::TextSecond));
        painter->drawText(QRect(area.right() - textWidth, area.top(), textWidth, area.height()),
                          Qt::AlignRight | Qt::AlignVCenter, text);
        painter->restore();
    }
};

/// 「建议」列的徽章：可清理（绿）/ 谨慎（黄）/ 受保护（灰）。
///
/// 画成带底色的胶囊而不是纯文字：这一列的目的是"扫一眼就知道哪些能删"，
/// 纯彩色文字在一屏 30 行里不够跳。颜色在 paint 时才取（不是建表时快照），
/// 所以运行中切换深浅主题也会跟着变。
class LevelBadgeDelegate : public QStyledItemDelegate
{
public:
    explicit LevelBadgeDelegate(QObject *parent)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        opt.text.clear();

        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        const int level = index.data(kLevelRole).toInt();
        if (text.isEmpty())
            return;

        QColor background;
        QColor foreground;
        switch (static_cast<diskscan::Level>(level)) {
        case diskscan::Level::Recommend:
            background = xtools::theme::color(xtools::theme::Color::SuccessBg);
            foreground = xtools::theme::color(xtools::theme::Color::SuccessText);
            break;
        case diskscan::Level::Caution:
            // 没有专门的 warn 底色令牌，用 warn 色加低透明度叠在主表面上：
            // 深色主题下也能自然压暗，不会像实色块那样刺眼
            background = xtools::theme::color(xtools::theme::Color::Warn);
            background.setAlpha(38);
            foreground = xtools::theme::color(xtools::theme::Color::WarnText);
            break;
        case diskscan::Level::Protected:
            background = xtools::theme::color(xtools::theme::Color::BgSubtle);
            foreground = xtools::theme::color(xtools::theme::Color::TextMuted);
            break;
        case diskscan::Level::Normal:
            return;
        }

        QFont font = option.font;
        font.setPointSizeF(qMax(7.0, font.pointSizeF() - 1.0));
        const QFontMetrics metrics(font);
        const int width = metrics.horizontalAdvance(text) + 12;

        QRect badge(option.rect.left() + 8, option.rect.center().y() - 9, width, 18);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(background);
        painter->drawRoundedRect(badge, 4, 4);
        painter->setPen(foreground);
        painter->setFont(font);
        painter->drawText(badge, Qt::AlignCenter, text);
        painter->restore();
    }
};

/// 把路径里的 "C:/Users/x" 换成系统习惯的分隔符，只为显示；
/// 内部比较与文件操作一律用 Qt 的 "/" 形式，避免混用两种分隔符时判断出错。
QString displayPath(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

} // namespace

DiskScanPanel::DiskScanPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐（与其他插件面板一致）
    root->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    root->setSpacing(12);

    // ---------------------------------------------------------------- 概览卡
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("dsCard"));
    auto *cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(18, 12, 18, 12);
    cardLayout->setSpacing(18);

    auto *percentBox = new QVBoxLayout;
    percentBox->setSpacing(0);
    auto *caption = new QLabel(QStringLiteral("磁盘占用"), card);
    caption->setObjectName(QStringLiteral("dsCaption"));
    m_percent = new QLabel(QStringLiteral("—"), card);
    m_percent->setObjectName(QStringLiteral("dsPercent"));
    percentBox->addWidget(caption);
    percentBox->addWidget(m_percent);
    percentBox->addStretch(1);
    cardLayout->addLayout(percentBox);

    auto *barBox = new QVBoxLayout;
    barBox->setSpacing(8);
    m_used = new QLabel(QStringLiteral("正在读取磁盘信息…"), card);
    m_used->setObjectName(QStringLiteral("dsUsed"));
    m_diskBar = new QProgressBar(card);
    m_diskBar->setObjectName(QStringLiteral("dsDiskBar"));
    m_diskBar->setRange(0, 100);
    m_diskBar->setTextVisible(false);
    m_diskBar->setFixedHeight(6);
    barBox->addStretch(1);
    barBox->addWidget(m_used);
    barBox->addWidget(m_diskBar);
    barBox->addStretch(1);
    cardLayout->addLayout(barBox, 1);

    m_avail = new QLabel(QStringLiteral("剩余 —"), card);
    m_avail->setObjectName(QStringLiteral("dsAvail"));
    cardLayout->addWidget(m_avail, 0, Qt::AlignRight | Qt::AlignVCenter);
    root->addWidget(card);

    // ---------------------------------------------------------------- 操作条
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    m_volume = new QComboBox(this);
    m_volume->setObjectName(QStringLiteral("dsVolume"));
    m_volume->setMinimumWidth(190);
    m_volume->setMaximumWidth(280);
    m_volume->setToolTip(QStringLiteral("选择要统计的磁盘（第一项是用户主目录）"));
    toolbar->addWidget(m_volume);
    m_comboArrow = new ComboArrow(m_volume);
    m_comboArrow->setObjectName(QStringLiteral("dsComboArrow"));
    m_volume->installEventFilter(this);

    m_up = new QPushButton(QStringLiteral("上一级"), this);
    m_up->setObjectName(QStringLiteral("dsUp"));
    m_up->setEnabled(false);
    toolbar->addWidget(m_up);

    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("dsPath"));
    m_path->setPlaceholderText(QStringLiteral("输入或粘贴文件夹路径，回车扫描"));
    m_path->setClearButtonEnabled(true);
    toolbar->addWidget(m_path, 1);

    m_browse = new QPushButton(QStringLiteral("浏览…"), this);
    m_browse->setObjectName(QStringLiteral("dsBrowse"));
    toolbar->addWidget(m_browse);

    m_scan = new QPushButton(QStringLiteral("扫描"), this);
    m_scan->setObjectName(QStringLiteral("dsScan"));
    m_scan->setProperty("accent", true);
    toolbar->addWidget(m_scan);

    m_stop = new QPushButton(QStringLiteral("停止"), this);
    m_stop->setObjectName(QStringLiteral("dsStop"));
    m_stop->setEnabled(false);
    toolbar->addWidget(m_stop);

    // 「找大头」是表出来之后的第二步动作，所以排在「扫描」后面、不抢视觉焦点。
    // 它做的是一次额外遍历，不是每层重扫：见 DiskScan.h 的 findHotspot。
    m_hotspot = new QPushButton(QStringLiteral("找大头"), this);
    m_hotspot->setObjectName(QStringLiteral("dsHotspot"));
    m_hotspot->setEnabled(false);
    m_hotspot->setToolTip(QStringLiteral("从当前位置顺着最大的子目录一直往下，"
                                         "直接定位到真正吃掉空间的那一层，并告诉你它是什么、该怎么清"));
    toolbar->addWidget(m_hotspot);

    root->addLayout(toolbar);

    // ---------------------------------------------------------------- 扫描进度
    m_scanBar = new QProgressBar(this);
    m_scanBar->setObjectName(QStringLiteral("dsScanBar"));
    m_scanBar->setTextVisible(false);
    m_scanBar->setFixedHeight(4);
    m_scanBar->setRange(0, 0);   // 子项数未定时走不确定态
    m_scanBar->setVisible(false);
    root->addWidget(m_scanBar);

    // ---------------------------------------------------------------- 面包屑
    auto *crumbHost = new QWidget(this);
    crumbHost->setObjectName(QStringLiteral("dsCrumbs"));
    m_crumbLayout = new QHBoxLayout(crumbHost);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(2);
    root->addWidget(crumbHost);

    // ---------------------------------------------------------------- 筛选条
    auto *filters = new QHBoxLayout;
    filters->setSpacing(14);
    m_onlyCleanable = new QCheckBox(QStringLiteral("只看建议清理的项"), this);
    m_onlyCleanable->setObjectName(QStringLiteral("dsOnlyCleanable"));
    m_onlyCleanable->setToolTip(
        QStringLiteral("只留下缓存 / 临时 / 日志 / node_modules 这类可以放心删的目录"));
    filters->addWidget(m_onlyCleanable);

    m_hideSmall = new QCheckBox(QStringLiteral("隐藏小于 50 MB"), this);
    m_hideSmall->setObjectName(QStringLiteral("dsHideSmall"));
    m_hideSmall->setToolTip(QStringLiteral("整盘扫描时小目录成千上万，先把它们藏起来看大户"));
    filters->addWidget(m_hideSmall);

    // 汇总标签吃掉"剩下的全部宽度"并右对齐，而不是让它按文字长度自适宽：
    // 扫描中它每 120ms 被改写一次，宽度跟着文字变会让这一行左右跳，
    // 极端情况下（文字刚变长、布局还没重算）只露出最后一个字。
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("dsSummary"));
    m_summary->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    filters->addWidget(m_summary, 1);

    // 一键清理：只作用于"列表里当前显示、且标了可清理"的项，所见即所清，
    // 免得把被筛选藏起来的东西一起删掉。真正的删除仍走 trashItems 的二次确认。
    m_cleanAll = new QPushButton(this);
    m_cleanAll->setObjectName(QStringLiteral("dsCleanAll"));
    m_cleanAll->setProperty("compact", true);
    m_cleanAll->setEnabled(false);
    m_cleanAll->setVisible(false);
    m_cleanAll->setToolTip(QStringLiteral("把列表里标了「可清理」的项移到回收站（会先确认）"));
    filters->addWidget(m_cleanAll);
    root->addLayout(filters);

    // ---------------------------------------------------------------- 大头条
    // 只在按过「找大头」之后出现：没有这条链路时它不该占一行高度
    m_hotBanner = new QFrame(this);
    m_hotBanner->setObjectName(QStringLiteral("dsHotBanner"));
    auto *hotLayout = new QVBoxLayout(m_hotBanner);
    hotLayout->setContentsMargins(14, 9, 14, 9);
    hotLayout->setSpacing(3);
    m_hotChain = new QLabel(m_hotBanner);
    m_hotChain->setObjectName(QStringLiteral("dsHotChain"));
    m_hotChain->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_hotHint = new QLabel(m_hotBanner);
    m_hotHint->setObjectName(QStringLiteral("dsHotHint"));
    m_hotHint->setWordWrap(true);
    hotLayout->addWidget(m_hotChain);
    hotLayout->addWidget(m_hotHint);
    m_hotBanner->setVisible(false);
    root->addWidget(m_hotBanner);

    // ---------------------------------------------------------------- 明细表
    m_stack = new QStackedWidget(this);

    m_empty = new QLabel(QStringLiteral("选择一块磁盘或文件夹，点「扫描」开始统计\n\n"
                                        "只统计文件大小，读不进去的系统目录会标注出来"),
                         m_stack);
    m_empty->setObjectName(QStringLiteral("dsEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_stack->addWidget(m_empty);

    m_table = new QTreeWidget(m_stack);
    m_table->setObjectName(QStringLiteral("dsTable"));
    m_table->setColumnCount(6);
    m_table->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("大小"),
                              QStringLiteral("占比"), QStringLiteral("文件数"),
                              QStringLiteral("说明"), QStringLiteral("建议")});
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 多选是为了"一次清几个缓存目录"：单选会逼用户来回点三遍
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setItemDelegate(new RowHeightDelegate(m_table));
    m_table->setItemDelegateForColumn(ColShare, new ShareBarDelegate(m_table));
    m_table->setItemDelegateForColumn(ColLevel, new LevelBadgeDelegate(m_table));
    m_table->headerItem()->setToolTip(
        ColShare, QStringLiteral("该文件夹 / 文件占当前这一层合计体积的比例"));
    m_table->headerItem()->setToolTip(
        ColHint, QStringLiteral("这一项是什么、该怎么处理。按目录名 / 路径的经验规则给出，仅供参考；"
                                "完整文案在悬浮提示里"));
    m_table->headerItem()->setToolTip(
        ColLevel, QStringLiteral("可清理：缓存 / 临时 / 日志；谨慎：构建产物、备份、个人资料；"
                                 "受保护：系统目录，不提供清理入口"));

    QHeaderView *header = m_table->header();
    header->setFixedHeight(30);
    header->setStretchLastSection(false);
    header->setSectionsClickable(true);
    header->setSortIndicatorShown(true);
    // 名称给固定宽度（最长也就 java_error_in_idea.hprof 这种），剩下全给「说明」：
    // 名字列跟着窗口拉宽只是多留白，说明才是这屏里信息量最大的列。
    // 名称设成 Interactive 而不是 Fixed —— 个别超长名字用户还能自己拖宽。
    header->setSectionResizeMode(ColName, QHeaderView::Interactive);
    header->setSectionResizeMode(ColHint, QHeaderView::Stretch);
    for (const int column : {ColSize, ColShare, ColFiles, ColLevel})
        header->setSectionResizeMode(column, QHeaderView::Fixed);
    // 宽度按最长的真实数据定：大小要装下 "≥ 123.4 GB"，文件数要装下 "1,234,567"，
    // 建议列要装下 "可清理" 胶囊
    header->resizeSection(ColName, 232);
    header->resizeSection(ColSize, 106);
    header->resizeSection(ColShare, 160);
    header->resizeSection(ColFiles, 96);
    header->resizeSection(ColLevel, 78);
    for (const int column : {ColSize, ColShare, ColFiles})
        m_table->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    // 「建议」列的徽章是从格子左边 8px 画的，表头默认居中会跟它对不齐
    m_table->headerItem()->setTextAlignment(ColLevel, Qt::AlignLeft | Qt::AlignVCenter);
    header->setSortIndicator(ColSize, Qt::DescendingOrder);
    m_stack->addWidget(m_table);

    root->addWidget(m_stack, 1);

    // ---------------------------------------------------------------- 底部
    auto *footer = new QHBoxLayout;
    footer->setSpacing(10);
    m_status = new QLabel(QStringLiteral("准备就绪"), this);
    m_status->setObjectName(QStringLiteral("dsStatus"));
    footer->addWidget(m_status, 1);

    auto *hint = new QLabel(QStringLiteral("双击进入文件夹 · 右键可打开或清理"), this);
    hint->setObjectName(QStringLiteral("dsHint"));
    footer->addWidget(hint);

    m_reveal = new QPushButton(QStringLiteral("在文件管理器中打开"), this);
    m_reveal->setObjectName(QStringLiteral("dsReveal"));
    m_reveal->setEnabled(false);
    footer->addWidget(m_reveal);

    m_trash = new QPushButton(QStringLiteral("移到回收站"), this);
    m_trash->setObjectName(QStringLiteral("dsTrash"));
    m_trash->setEnabled(false);
    footer->addWidget(m_trash);
    root->addLayout(footer);

    // ---------------------------------------------------------------- 初始状态
    m_root = QDir::homePath();
    m_path->setText(displayPath(m_root));
    reloadVolumes();
    updateSummary();
    rebuildCrumbs();
    rebuildTable();

    // ---------------------------------------------------------------- 连接
    // 先填好下拉框再连信号：否则 addItem 的过程会触发一次"扫到一半的磁盘"
    connect(m_volume, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        const QString path = m_volume->currentData().toString();
        if (!path.isEmpty())
            startScan(path);
    });
    connect(m_up, &QPushButton::clicked, this, &DiskScanPanel::goUp);
    connect(m_browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择要统计的文件夹"), m_root.isEmpty() ? QDir::homePath() : m_root);
        if (!path.isEmpty())
            startScan(path);
    });
    connect(m_scan, &QPushButton::clicked, this, [this] {
        const QString typed = QDir::fromNativeSeparators(m_path->text().trimmed());
        startScan(typed.isEmpty() ? m_root : typed);
    });
    connect(m_path, &QLineEdit::returnPressed, m_scan, &QPushButton::click);   // 回车即扫描
    connect(m_stop, &QPushButton::clicked, this, &DiskScanPanel::cancelScan);
    connect(m_hotspot, &QPushButton::clicked, this, &DiskScanPanel::findHotspot);
    connect(m_cleanAll, &QPushButton::clicked, this, &DiskScanPanel::cleanCleanable);
    connect(m_reveal, &QPushButton::clicked, this, &DiskScanPanel::revealSelected);
    connect(m_trash, &QPushButton::clicked, this, &DiskScanPanel::trashSelected);
    connect(m_onlyCleanable, &QCheckBox::toggled, this, [this] {
        rebuildTable();
        updateSelectionState();
    });
    connect(m_hideSmall, &QCheckBox::toggled, this, [this] {
        rebuildTable();
        updateSelectionState();
    });
    connect(m_table, &QTreeWidget::itemSelectionChanged, this, &DiskScanPanel::updateSelectionState);
    connect(m_table, &QTreeWidget::itemActivated, this, &DiskScanPanel::activateItem);
    connect(m_table, &QTreeWidget::customContextMenuRequested, this, &DiskScanPanel::showContextMenu);
    connect(header, &QHeaderView::sectionClicked, this, &DiskScanPanel::applySortColumn);

    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, [this] {
        rebuildTable();
        updateSelectionState();
    });
}

DiskScanPanel::~DiskScanPanel()
{
    // 宿主可能在扫描途中销毁面板（回首页 / 关闭独立窗口）。先让 worker 尽快退出，
    // 再等它结束：worker 只做纯计算，quit() 打断不了它，只能靠取消标志 + wait()。
    // 期间它投递到 UI 线程的调用会在本对象析构时被 Qt 丢掉，不会碰已释放的内存。
    if (m_cancel)
        m_cancel->store(true);
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    if (m_hotThread) {
        m_hotThread->wait();
        delete m_hotThread;
        m_hotThread = nullptr;
    }
}

void DiskScanPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 只在第一次显示时自动扫：点页头「独立窗口」会把面板搬到另一个顶层窗口，
    // 那也会触发 showEvent，重扫一遍既慢又让人莫名其妙
    if (m_hasScanned)
        return;
    m_hasScanned = true;
    startScan(m_root);
}

bool DiskScanPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_volume && m_comboArrow) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show: {
            // 与 qss 里 ::drop-down 的 18px 箭头区对齐；窗口拉伸、字体缩放后都要重摆
            const QRect box = m_volume->rect();
            m_comboArrow->move(box.width() - 18 + 4,
                               (box.height() - m_comboArrow->height()) / 2);
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

void DiskScanPanel::reloadVolumes()
{
    m_volume->blockSignals(true);
    m_volume->clear();

    // 主目录固定第一项：它是绝大多数人真正想清的地方，也不依赖挂载点枚举
    const QStorageInfo homeStorage(QDir::homePath());
    m_volume->addItem(homeStorage.isValid()
                          ? QStringLiteral("主目录 · 剩 %1").arg(
                                diskscan::formatBytes(homeStorage.bytesAvailable()))
                          : QStringLiteral("主目录"),
                      QDir::homePath());
    m_volume->setItemData(0, displayPath(QDir::homePath()), Qt::ToolTipRole);

    QVector<QStorageInfo> volumes;
    for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
        // 只列真正能放的磁盘：只读挂载（光盘、squashfs 镜像）没有清理价值，
        // Linux 上还会一次冒出几十个（snap 每个版本一个 loop 设备）
        if (!storage.isValid() || !storage.isReady())
            continue;
        if (storage.isReadOnly() || storage.bytesTotal() <= 0)
            continue;
        volumes.append(storage);
    }
    std::sort(volumes.begin(), volumes.end(), [](const QStorageInfo &a, const QStorageInfo &b) {
        return a.rootPath().localeAwareCompare(b.rootPath()) < 0;
    });

    for (const QStorageInfo &storage : volumes) {
        m_volume->addItem(
            QStringLiteral("%1 · 剩 %2").arg(displayPath(storage.rootPath()),
                                             diskscan::formatBytes(storage.bytesAvailable())),
            storage.rootPath());
        const QString name = storage.displayName();
        m_volume->setItemData(m_volume->count() - 1,
                              name.isEmpty() ? displayPath(storage.rootPath()) : name,
                              Qt::ToolTipRole);
    }

    m_volume->blockSignals(false);
    syncVolumeTo(m_root);
}

void DiskScanPanel::syncVolumeTo(const QString &path)
{
    if (path.isEmpty())
        return;

    // 找包含该路径、且挂载点最长的那一项：/home 与 / 都能匹配 /home/me，
    // 但用户在下拉框里想看到的是更具体的那个
    int best = -1;
    int bestLength = -1;
    for (int i = 0; i < m_volume->count(); ++i) {
        const QString root = m_volume->itemData(i).toString();
        if (root.isEmpty() || !diskscan::isSameOrInside(path, root))
            continue;
        if (root.size() > bestLength) {
            bestLength = root.size();
            best = i;
        }
    }
    if (best < 0)
        return;

    // 这一步只是"让下拉框对上当前路径"，不能再触发一次扫描
    const QSignalBlocker blocker(m_volume);
    m_volume->setCurrentIndex(best);
}

void DiskScanPanel::startScan(const QString &path)
{
    if (m_scanning)
        return;   // 扫描中改目标：先按「停止」，避免两个 worker 抢同一份 m_items

    const QFileInfo info(path);
    if (!info.exists()) {
        setStatus(QStringLiteral("路径不存在：%1").arg(displayPath(path)), true);
        return;
    }
    if (!info.isDir()) {
        setStatus(QStringLiteral("只能扫描文件夹（这是一个文件）：%1").arg(displayPath(path)), true);
        return;
    }

    m_root = QDir::cleanPath(info.absoluteFilePath());
    m_path->setText(displayPath(m_root));
    syncVolumeTo(m_root);
    updateSummary();
    rebuildCrumbs();
    m_up->setEnabled(!QDir(m_root).isRoot());

    m_items.clear();
    m_done = 0;
    m_total = 0;
    // 换了位置，旧的大头链路就不作数了；找大头自己会在 startScan 之后重新显示它
    if (m_hotBanner)
        m_hotBanner->setVisible(false);
    rebuildTable();
    updateSelectionState();

    m_scanBar->setRange(0, 0);   // 还不知道有几项，先走不确定态
    m_scanBar->setVisible(true);
    m_scanning = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    setBusy(true);
    setStatus(QStringLiteral("正在统计：%1").arg(displayPath(m_root)), false);

    auto cancel = m_cancel;
    auto result = QSharedPointer<diskscan::Result>::create();
    const QString rootPath = m_root;

    // 结果放共享指针、进度靠 invokeMethod 排队：worker 全程不碰任何成员与控件。
    // 这两条是"面板随时可能被销毁"的前提（见析构函数）。
    QThread *thread = QThread::create([this, rootPath, cancel, result] {
        *result = diskscan::scanChildren(
            rootPath, cancel.get(),
            [this, cancel](const diskscan::Progress &progress) {
                if (cancel->load())
                    return;
                const diskscan::Progress copy = progress;
                QMetaObject::invokeMethod(this, [this, copy] {
                    m_done = copy.done;
                    m_total = copy.total;
                    if (m_scanBar->maximum() != copy.total && copy.total > 0)
                        m_scanBar->setRange(0, copy.total);
                    if (copy.total > 0)
                        m_scanBar->setValue(copy.done);

                    const QString current = QFileInfo(copy.current).fileName();
                    setStatus(QStringLiteral("正在统计 %1/%2：%3 · 已扫到 %4")
                                  .arg(m_done)
                                  .arg(m_total)
                                  .arg(current.isEmpty() ? QStringLiteral("…") : current,
                                       diskscan::formatBytes(copy.bytes)),
                              false);
                }, Qt::QueuedConnection);
            },
            [this, cancel](const diskscan::Item &item) {
                if (cancel->load())
                    return;
                const diskscan::Item copy = item;
                QMetaObject::invokeMethod(this, [this, copy] { appendItem(copy); },
                                          Qt::QueuedConnection);
            });
    });

    m_thread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread, result] {
        if (m_thread == thread)
            m_thread = nullptr;
        applyScanFinished(*result);
    });
    thread->start();
}

void DiskScanPanel::cancelScan()
{
    if (!m_scanning || !m_cancel)
        return;
    m_cancel->store(true);
    m_stop->setEnabled(false);
    setStatus(QStringLiteral("正在停止…"), false);
}

void DiskScanPanel::appendItem(const diskscan::Item &item)
{
    m_items.append(item);
    // 节流：一次目录里几十项时不必每项都重建整表
    if (m_flushTimer && !m_flushTimer->isActive())
        m_flushTimer->start();
}

void DiskScanPanel::applyScanFinished(const diskscan::Result &result)
{
    if (m_flushTimer)
        m_flushTimer->stop();

    m_scanning = false;
    m_cancel.reset();
    m_scanBar->setVisible(false);
    setBusy(false);

    if (!result.error.isEmpty()) {
        m_items.clear();
        rebuildTable();
        updateSelectionState();
        m_pendingStatus.clear();   // 扫描失败比"刚才清理成功"更重要
        setStatus(result.error, true);
        return;
    }

    // worker 期间 appendItem 已经陆续填了同一份数据；这里用最终结果统一覆盖，
    // 保证"取消"时也不会剩下半条重复记录（result.items 只含已算完的项）
    m_items = result.items;
    rebuildTable();
    if (!m_pendingSelectPath.isEmpty()) {
        selectPath(m_pendingSelectPath);
        m_pendingSelectPath.clear();
    }
    updateSelectionState();

    if (result.canceled) {
        setStatus(QStringLiteral("已停止：统计到 %1 项，合计 %2")
                      .arg(m_items.size())
                      .arg(diskscan::formatBytes(result.totalBytes)),
                  false);
        applyPendingStatus();
        return;
    }

    QString text = QStringLiteral("共 %1 项，合计 %2 · %3 个文件 · 用时 %4 秒")
                       .arg(m_items.size())
                       .arg(diskscan::formatBytes(result.totalBytes),
                            diskscan::formatCount(result.totalFiles),
                            QString::number(result.elapsedMs / 1000.0, 'f', 1));
    if (result.unreadableDirs > 0) {
        text += QStringLiteral(" · %1 个目录无权限读取，体积可能偏小")
                    .arg(result.unreadableDirs);
    }
    setStatus(text, false);
    applyPendingStatus();
}

void DiskScanPanel::applyPendingStatus()
{
    if (m_pendingStatus.isEmpty())
        return;
    setStatus(m_pendingStatus, m_pendingWarn);
    m_pendingStatus.clear();
    m_pendingWarn = false;
}

void DiskScanPanel::rebuildTable()
{
    const diskscan::Item *keep = selectedItem();
    const QString keepPath = keep ? keep->path : QString();
    const int scroll = m_table->verticalScrollBar()->value();

    const bool onlyCleanable = m_onlyCleanable->isChecked();
    const bool hideSmall = m_hideSmall->isChecked();

    // total 用**未筛选**的全部子项：占比回答的是"这一层里它占多少"，
    // 跟着筛选条件变会让同一个文件夹在勾选前后显示两个百分比
    quint64 total = 0;
    for (const diskscan::Item &item : m_items)
        total += item.bytes;

    QVector<diskscan::Item> rows;
    rows.reserve(m_items.size());
    for (const diskscan::Item &item : m_items) {
        if (onlyCleanable && item.level != diskscan::Level::Recommend)
            continue;
        if (hideSmall && item.bytes < kSmallItemBytes)
            continue;
        rows.append(item);
    }

    const int column = m_sortColumn;
    const bool descending = m_sortDescending;
    std::stable_sort(rows.begin(), rows.end(), [column, descending](const diskscan::Item &a,
                                                                  const diskscan::Item &b) {
        if (column == ColSize && a.bytes != b.bytes)
            return descending ? a.bytes > b.bytes : a.bytes < b.bytes;
        if (column == ColFiles && a.files != b.files)
            return descending ? a.files > b.files : a.files < b.files;
        if (column == ColLevel && a.level != b.level) {
            // 等级本身没有大小之分，按"可清理 → 谨慎 → 普通 → 受保护"的实用顺序排，
            // 这样点一下「建议」列就能把能删的顶到最上面
            const int rankA = a.level == diskscan::Level::Recommend ? 0
                            : a.level == diskscan::Level::Caution ? 1
                            : a.level == diskscan::Level::Normal ? 2 : 3;
            const int rankB = b.level == diskscan::Level::Recommend ? 0
                            : b.level == diskscan::Level::Caution ? 1
                            : b.level == diskscan::Level::Normal ? 2 : 3;
            return descending ? rankA > rankB : rankA < rankB;
        }
        if (column == ColName) {
            const int cmp = QString::localeAwareCompare(a.name, b.name);
            if (cmp != 0)
                return descending ? cmp > 0 : cmp < 0;
        }
        // 指标相等（或正按名字排）时用名字兜底：否则每次刷新同一批条目会跳序
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });

    m_table->setUpdatesEnabled(false);
    m_table->clear();

    m_visibleCleanable = 0;
    m_visibleCleanableBytes = 0;

    QTreeWidgetItem *restore = nullptr;
    for (const diskscan::Item &row : rows) {
        auto *item = new QTreeWidgetItem(m_table);
        item->setData(ColName, kPathRole, row.path);
        item->setData(ColName, kIsDirRole, row.isDir);
        item->setData(ColLevel, kLevelRole, static_cast<int>(row.level));
        if (row.isDir)
            item->setIcon(ColName, m_table->style()->standardIcon(QStyle::SP_DirIcon));
        item->setText(ColName, row.name);

        // 有子目录读不进去时体积只是下限，用 "≥" 明说，别让用户拿它当准数
        const QString sizeText = diskscan::formatBytes(row.bytes);
        item->setText(ColSize, row.unreadable ? QStringLiteral("≥ %1").arg(sizeText) : sizeText);
        item->setText(ColFiles, diskscan::formatCount(row.files));
        item->setText(ColLevel, diskscan::levelLabel(row.level));
        item->setText(ColHint, row.hint);

        const double fraction = total > 0 ? static_cast<double>(row.bytes) / total : 0.0;
        item->setText(ColShare, fraction > 0.0
                                    ? QStringLiteral("%1%").arg(fraction * 100.0, 0, 'f', 1)
                                    : QStringLiteral("—"));
        item->setData(ColShare, kShareRole, fraction);

        for (const int col : {ColSize, ColShare, ColFiles})
            item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);

        // 受保护的项整行压暗：它在这一屏里只是"给你看的"，不是候选操作对象
        if (row.level == diskscan::Level::Protected) {
            const QBrush dim(xtools::theme::color(xtools::theme::Color::TextFaint));
            for (int col = 0; col < m_table->columnCount(); ++col)
                item->setForeground(col, dim);
        }

        // 说明列按语气上色，放在压暗之后：受保护项的说明要看得见（"别直接删"这种话
        // 恰恰是给最该被劝住的人看的），不能被整行压暗吃掉
        if (!row.hint.isEmpty()) {
            QColor tone;
            switch (row.hintTone) {
            case diskscan::Level::Recommend: tone = xtools::theme::color(xtools::theme::Color::SuccessText); break;
            case diskscan::Level::Caution:   tone = xtools::theme::color(xtools::theme::Color::WarnText); break;
            case diskscan::Level::Protected: tone = xtools::theme::color(xtools::theme::Color::TextMuted); break;
            case diskscan::Level::Normal:    tone = xtools::theme::color(xtools::theme::Color::TextMuted); break;
            }
            item->setForeground(ColHint, QBrush(tone));
        }

        QString tip = displayPath(row.path);
        tip += QStringLiteral("\n%1 · %2 个文件")
                   .arg(row.isDir ? QStringLiteral("文件夹") : QStringLiteral("文件"),
                        diskscan::formatCount(row.files));
        if (row.unreadableDirs > 0) {
            tip += QStringLiteral("\n%1 个子目录无权限读取，实际体积可能更大")
                       .arg(row.unreadableDirs);
        }
        if (row.skippedLinks > 0) {
            tip += QStringLiteral("\n跳过 %1 个符号链接（不跟进，避免绕圈与重复计数）")
                       .arg(row.skippedLinks);
        }
        if (row.level == diskscan::Level::Protected)
            tip += QStringLiteral("\n系统目录或用户资料根目录，不提供清理入口");
        if (!row.hint.isEmpty())
            tip += QLatin1Char('\n') + row.hint;
        item->setToolTip(ColName, tip);
        item->setToolTip(ColHint, row.hint);

        if (row.level == diskscan::Level::Recommend) {
            ++m_visibleCleanable;
            m_visibleCleanableBytes += row.bytes;
        }

        if (!keepPath.isEmpty() && row.path == keepPath)
            restore = item;
    }

    m_table->setUpdatesEnabled(true);

    if (restore) {
        restore->setSelected(true);
        m_table->setCurrentItem(restore);
    }
    // 滚动位置按新的最大值夹一下：条目变少时直接设旧值会被吞掉
    m_table->verticalScrollBar()->setValue(
        qMin(scroll, m_table->verticalScrollBar()->maximum()));

    const bool empty = m_table->topLevelItemCount() == 0;
    m_stack->setCurrentWidget(empty ? static_cast<QWidget *>(m_empty)
                                    : static_cast<QWidget *>(m_table));
    if (empty) {
        if (m_scanning)
            m_empty->setText(QStringLiteral("正在统计…"));
        else if (!m_items.isEmpty())
            m_empty->setText(QStringLiteral("当前筛选条件下没有可显示的项\n\n"
                                            "取消上面的勾选，或者换一个位置"));
        else
            m_empty->setText(QStringLiteral("这个文件夹是空的，或者里面的项目都读不到\n\n"
                                            "换一个位置，或点「上一级」回去"));
    }

    updateFilterSummary();
}

void DiskScanPanel::updateFilterSummary()
{
    quint64 total = 0;
    quint64 cleanable = 0;
    int cleanableCount = 0;
    for (const diskscan::Item &item : m_items) {
        total += item.bytes;
        if (item.level == diskscan::Level::Recommend) {
            ++cleanableCount;
            cleanable += item.bytes;
        }
    }

    if (m_items.isEmpty()) {
        m_summary->setText(QString());
        m_summary->setToolTip(QString());
        m_cleanAll->setVisible(false);
        m_cleanAll->setEnabled(false);
        return;
    }

    // 这一行只放"能捡回多少"这一个结论，写得越短越好：它和两个复选框挤在一行，
    // 长文案在窄窗口里会被截断（QLabel 会直接裁字，不留省略号）。
    // "共几项 / 合计多少"在底部状态栏里已经有了，不在这里重复。
    QString text = cleanableCount > 0
                       ? QStringLiteral("可清理 %1 项 · 约 %2")
                             .arg(cleanableCount)
                             .arg(diskscan::formatBytes(cleanable))
                       : QStringLiteral("这一层没有可清理的项");
    const int shown = m_table->topLevelItemCount();
    if (shown != m_items.size())
        text += QStringLiteral(" ｜ 显示 %1 / %2 项").arg(shown).arg(m_items.size());
    m_summary->setText(text);

    // 一键清理按钮跟着"看得见的可清理项"走：没有就整块收起，别留个点不动的灰按钮
    const bool canClean = m_visibleCleanable > 0 && !m_scanning;
    m_cleanAll->setVisible(canClean);
    m_cleanAll->setEnabled(canClean);
    if (canClean) {
        m_cleanAll->setText(QStringLiteral("一键清理 %1 项").arg(m_visibleCleanable));
        m_cleanAll->setToolTip(QStringLiteral("把列表里这 %1 项（约 %2）移到回收站，会先弹确认框")
                                   .arg(m_visibleCleanable)
                                   .arg(diskscan::formatBytes(m_visibleCleanableBytes)));
    }

    // 完整口径放悬浮提示：想看总数的人不用去数列表
    m_summary->setToolTip(QStringLiteral("共 %1 项 · 合计 %2 · 建议清理 %3 项约 %4")
                              .arg(m_items.size())
                              .arg(diskscan::formatBytes(total))
                              .arg(cleanableCount)
                              .arg(diskscan::formatBytes(cleanable)));
}

void DiskScanPanel::rebuildCrumbs()
{
    // 逐段拆出"可点击的绝对路径"。用 QFileInfo 往上走而不是按 '/' 切字符串：
    // Windows 的 "C:/" 与 POSIX 的 "/" 在切分后首段分别是 "C:" 和空串，
    // 自己拼容易拼出 "/C:" 这种不存在的路径。
    QVector<QPair<QString, QString>> segments;   // (显示名, 绝对路径)
    QString current = m_root;
    int guard = 0;
    while (!current.isEmpty() && guard++ < 64) {
        const QString name = QFileInfo(current).fileName();
        if (name.isEmpty()) {   // 走到盘根：QFileInfo("C:/").fileName() 是空
            segments.prepend({displayPath(current), current});
            break;
        }
        segments.prepend({name, current});
        const QString parent = QFileInfo(current).absolutePath();
        if (diskscan::canonicalForCompare(parent) == diskscan::canonicalForCompare(current))
            break;
        current = parent;
    }

    // 清空旧段：只从布局里摘掉不销毁的话，控件会浮在左上角
    while (QLayoutItem *entry = m_crumbLayout->takeAt(0)) {
        if (QWidget *widget = entry->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete entry;
    }

    if (segments.isEmpty())
        return;

    const auto addSeparator = [this] {
        auto *sep = new QLabel(QStringLiteral("›"), this);
        sep->setObjectName(QStringLiteral("dsCrumbSep"));
        m_crumbLayout->addWidget(sep);
    };
    const auto addLink = [this](const QString &label, const QString &target) {
        auto *button = new QPushButton(label, this);
        // link 属性在 style.qss 里被渲染成"像链接的按钮"：透明底、无边框、强调色文字。
        // 不用 QLabel 的富文本 <a>：那种链接色取自 palette，深色主题下对比度不够，
        // 而且没法跟着宿主换肤重算。
        button->setProperty("link", true);
        button->setProperty("compact", true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, [this, target] { startScan(target); });
        m_crumbLayout->addWidget(button);
    };

    // 深路径只留最后 5 段：段数一多会把这一行撑爆，而中间那几层基本没人点。
    // 开头的「…」跳到被折叠的最深一层，逐级后退仍然可行。
    int start = 0;
    if (segments.size() > 5) {
        const int hidden = segments.size() - 5;
        addLink(QStringLiteral("…"), segments.at(hidden - 1).second);
        addSeparator();
        start = hidden;
    }

    for (int i = start; i < segments.size(); ++i) {
        if (i > start)
            addSeparator();
        if (i == segments.size() - 1) {
            auto *here = new QLabel(segments.at(i).first, this);
            here->setObjectName(QStringLiteral("dsCrumbCurrent"));
            m_crumbLayout->addWidget(here);
        } else {
            addLink(segments.at(i).first, segments.at(i).second);
        }
    }
    m_crumbLayout->addStretch(1);
}

void DiskScanPanel::updateSummary()
{
    const QStorageInfo storage(m_root.isEmpty() ? QDir::homePath() : m_root);
    if (!storage.isValid() || !storage.isReady() || storage.bytesTotal() <= 0) {
        // 网络盘断开、权限受限的容器里会走到这里：给钉子文案，别留上一次的数字
        m_percent->setText(QStringLiteral("—"));
        m_used->setText(QStringLiteral("读不到这块磁盘的容量信息"));
        m_avail->setText(QStringLiteral("剩余 —"));
        m_diskBar->setValue(0);
        return;
    }

    const quint64 total = storage.bytesTotal();
    const quint64 free = storage.bytesAvailable();
    const quint64 used = total > free ? total - free : 0;
    const double percent = 100.0 * static_cast<double>(used) / static_cast<double>(total);

    m_percent->setText(QStringLiteral("%1%").arg(percent, 0, 'f', 1));
    m_used->setText(QStringLiteral("已用 %1 / 共 %2 · %3")
                        .arg(diskscan::formatBytes(used), diskscan::formatBytes(total),
                             displayPath(storage.rootPath())));
    m_avail->setText(QStringLiteral("剩余 %1").arg(diskscan::formatBytes(free)));
    m_diskBar->setValue(qRound(percent));
    m_used->setToolTip(QStringLiteral("按可用空间计算（Windows 上可能包含保留给系统还原等用途的空间）"));

    // 80% 起橙色、90% 起红色：与进程监控的警戒等级同一套观感
    const int level = percent >= 90.0 ? 2 : (percent >= 80.0 ? 1 : 0);
    if (level == m_lastLevel)
        return;
    m_lastLevel = level;
    const QString name = level >= 2 ? QStringLiteral("danger")
                                    : (level == 1 ? QStringLiteral("warn")
                                                  : QStringLiteral("normal"));
    for (QWidget *widget : {static_cast<QWidget *>(m_percent),
                            static_cast<QWidget *>(m_diskBar)}) {
        widget->setProperty("level", name);
        repolish(widget);
    }
}

void DiskScanPanel::updateSelectionState()
{
    const int count = m_scanning ? 0 : m_table->selectedItems().size();
    m_reveal->setEnabled(count > 0);
    m_trash->setEnabled(count > 0);
    m_trash->setText(count > 1 ? QStringLiteral("移到回收站 (%1)").arg(count)
                               : QStringLiteral("移到回收站"));
}

void DiskScanPanel::goUp()
{
    if (m_scanning || m_root.isEmpty())
        return;
    const QFileInfo info(m_root);
    const QString parent = info.absolutePath();
    // 盘根没有上一级：absolutePath() 对 "C:/" 返回 "C:/"、对 "/" 返回 "/"
    if (parent.isEmpty() || diskscan::canonicalForCompare(parent) == diskscan::canonicalForCompare(m_root))
        return;
    startScan(parent);
}

void DiskScanPanel::activateItem(QTreeWidgetItem *item)
{
    if (!item || m_scanning)
        return;
    const QString path = item->data(ColName, kPathRole).toString();
    if (item->data(ColName, kIsDirRole).toBool()) {
        startScan(path);
        return;
    }
    // 文件没有"下一层可看"，交给系统默认程序打开；打不开要说原因，不能静默
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        setStatus(QStringLiteral("打不开这个文件：%1").arg(displayPath(path)), true);
}

void DiskScanPanel::revealSelected()
{
    const diskscan::Item *item = selectedItem();
    if (!item)
        return;
    // 文件交给文件管理器时打开它所在的目录：直接 openUrl 文件会变成"用默认程序打开"，
    // 那是双击的语义，不是"在文件管理器中打开"
    const QString target = item->isDir ? item->path : QFileInfo(item->path).absolutePath();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(target)))
        setStatus(QStringLiteral("打不开这个位置：%1").arg(displayPath(target)), true);
}

void DiskScanPanel::trashSelected()
{
    QVector<diskscan::Item> items;
    const QList<QTreeWidgetItem *> selected = m_table->selectedItems();
    for (QTreeWidgetItem *row : selected) {
        if (const diskscan::Item *item = itemByPath(row->data(ColName, kPathRole).toString()))
            items.append(*item);
    }
    trashItems(items);
}

void DiskScanPanel::cleanCleanable()
{
    if (m_scanning)
        return;

    // 从**当前显示的行**反查数据项，而不是直接筛 m_items：
    // 按钮上写的"N 项"就是用户眼前这几行，所见即所清，不会把被筛选藏起来的项一起删
    QVector<diskscan::Item> items;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *row = m_table->topLevelItem(i);
        if (row->data(ColLevel, kLevelRole).toInt() != static_cast<int>(diskscan::Level::Recommend))
            continue;
        if (const diskscan::Item *item = itemByPath(row->data(ColName, kPathRole).toString()))
            items.append(*item);
    }
    if (items.isEmpty()) {
        setStatus(QStringLiteral("列表里没有标了「可清理」的项"), false);
        return;
    }
    trashItems(items);
}

void DiskScanPanel::trashItems(const QVector<diskscan::Item> &items)
{
    if (items.isEmpty())
        return;

    QStringList paths;
    QStringList advisories;   ///< 名字规则给出的提醒，确认框里再重复一次
    quint64 totalBytes = 0;
    for (const diskscan::Item &item : items) {
        // 保护目录是最后一道闸：界面可能因为选中态过期而指向了别的东西
        const QString blocked = diskscan::trashBlockReason(item.path);
        if (!blocked.isEmpty()) {
            setStatus(QStringLiteral("「%1」不能清理：%2").arg(item.name, blocked), true);
            return;
        }
        paths.append(item.path);
        totalBytes += item.bytes;
        // 确认框是用户唯一会读到"代价"的地方，把 "binlog 别直接删" 这类话搬过来
        if (!item.hint.isEmpty() && item.hintTone == diskscan::Level::Caution && advisories.size() < 3)
            advisories.append(QStringLiteral("·「%1」%2").arg(item.name, item.hint));
    }
    if (paths.isEmpty())
        return;

    // 二次确认，默认按钮是「取消」：清理 200 GB 的目录是这里唯一不可轻率的操作
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("移到回收站"));
    box.setIcon(QMessageBox::Warning);
    if (paths.size() == 1) {
        box.setText(QStringLiteral("把「%1」移到回收站？")
                        .arg(QFileInfo(paths.first()).fileName()));
    } else {
        box.setText(QStringLiteral("把这 %1 项移到回收站？").arg(paths.size()));
    }

    QString detail = QStringLiteral("合计约 %1。").arg(diskscan::formatBytes(totalBytes));
    if (paths.size() > 1) {
        QStringList preview;
        for (int i = 0; i < paths.size() && i < 5; ++i)
            preview.append(displayPath(paths.at(i)));
        if (paths.size() > 5)
            preview.append(QStringLiteral("…"));
        detail += QLatin1Char('\n') + preview.join(QLatin1Char('\n'));
    } else {
        detail += QLatin1Char('\n') + displayPath(paths.first());
    }
    detail += QStringLiteral("\n\n可以从系统回收站还原；清空回收站后空间才会真正释放。");
    // 回收站是按卷分配配额的：超过配额的删除会被系统直接永久抹掉，
    // 用户以为"反正能还原"的时候最容易出事，所以单独提示一句
    if (totalBytes >= kRecycleCapacityWarnBytes) {
        detail += QStringLiteral("\n\n体积已达 %1，可能超出回收站容量上限，"
                                 "系统有可能直接永久删除而不是放进回收站。"
                                 "请务必先确认它不是重要数据。")
                      .arg(diskscan::formatBytes(totalBytes));
    }
    if (!advisories.isEmpty()) {
        detail += QStringLiteral("\n\n先看清再动手：\n") + advisories.join(QLatin1Char('\n'));
    }
    box.setInformativeText(detail);

    QPushButton *confirm = box.addButton(QStringLiteral("移到回收站"), QMessageBox::DestructiveRole);
    QPushButton *cancel = box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != confirm)
        return;

    int succeeded = 0;
    QStringList failed;
    for (const QString &path : paths) {
        // QFile::moveToTrash 三平台都由 Qt 实现（Windows 走 Shell、macOS 走 Finder 的
        // 回收站、Linux 走 XDG trash 规范），不需要自己写平台分支。
        // 不做永久删除：这个工具的价值是"看得见再决定"，不是"一把火烧掉"。
        if (QFile::moveToTrash(path))
            ++succeeded;
        else
            failed.append(path);
    }

    // 体积变了，重新统计一遍当前目录（比手动从表里删行更可信）。
    // 结果文案先挂起：startScan 会把状态栏改成"正在统计…"，紧接着的进度回调
    // 还会不停覆盖它，等这一轮扫完再显示才不会一闪而过。
    if (failed.isEmpty()) {
        m_pendingStatus = QStringLiteral("已把 %1 项移到回收站（约 %2），清空回收站后空间才释放")
                              .arg(succeeded)
                              .arg(diskscan::formatBytes(totalBytes));
        m_pendingWarn = false;
    } else if (succeeded == 0) {
        m_pendingStatus = QStringLiteral("移到回收站失败：可能正被其他程序占用，或没有权限");
        m_pendingWarn = true;
    } else {
        m_pendingStatus = QStringLiteral("%1 项已移到回收站，%2 项失败（可能正被占用或权限不足）")
                              .arg(succeeded)
                              .arg(failed.size());
        m_pendingWarn = true;
    }
    startScan(m_root);
}

void DiskScanPanel::findHotspot()
{
    if (m_scanning || m_root.isEmpty())
        return;
    if (!QFileInfo(m_root).isDir()) {
        setStatus(QStringLiteral("只能分析文件夹：%1").arg(displayPath(m_root)), true);
        return;
    }

    m_scanning = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    setBusy(true);
    m_scanBar->setRange(0, 0);   // 没有"第几项"的概念，走不确定态
    m_scanBar->setVisible(true);
    setStatus(QStringLiteral("正在找大头：%1").arg(displayPath(m_root)), false);

    auto cancel = m_cancel;
    auto spot = QSharedPointer<diskscan::Hotspot>::create();
    const QString rootPath = m_root;

    // 与 startScan 同一套"worker 不碰成员"的约束：结果放共享指针，进度排队回 UI 线程
    QThread *thread = QThread::create([this, rootPath, cancel, spot] {
        *spot = diskscan::findHotspot(
            rootPath, cancel.get(), [this, cancel](const QString &current, quint64 scanned) {
                if (cancel->load())
                    return;
                const QString copy = current;
                QMetaObject::invokeMethod(this, [this, copy, scanned] {
                    const QString name = QFileInfo(copy).fileName();
                    setStatus(QStringLiteral("正在找大头 · 已扫 %1：%2")
                                  .arg(diskscan::formatBytes(scanned),
                                       name.isEmpty() ? QStringLiteral("…") : name),
                              false);
                }, Qt::QueuedConnection);
            });
    });

    m_hotThread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread, spot] {
        if (m_hotThread == thread)
            m_hotThread = nullptr;
        applyHotspotFinished(*spot);
    });
    thread->start();
}

void DiskScanPanel::applyHotspotFinished(const diskscan::Hotspot &spot)
{
    m_scanning = false;
    m_cancel.reset();
    m_scanBar->setVisible(false);
    setBusy(false);

    if (spot.canceled) {
        setStatus(QStringLiteral("已停止找大头"), false);
        return;
    }
    if (!spot.error.isEmpty()) {
        setStatus(spot.error, true);
        return;
    }
    if (spot.isEmpty()) {
        setStatus(QStringLiteral("这一层没有能再往下拆的子项"), false);
        return;
    }

    // 末端是文件夹就进去（下一步就是在里面勾选清理），是文件就停在上一层并选中它
    const QString leaf = spot.leafPath();
    const QString target = spot.isDir ? leaf : QFileInfo(leaf).absolutePath();
    const quint64 leafBytes = spot.sizes.last();

    m_pendingSelectPath = leaf;
    m_pendingWarn = false;
    if (spot.rootBytes > 0) {
        m_pendingStatus = QStringLiteral("大头：%1 · %2（占这一层的 %3%）")
                              .arg(spot.names.join(QStringLiteral(" › ")),
                                   diskscan::formatBytes(leafBytes),
                                   QString::number(100.0 * double(leafBytes) / double(spot.rootBytes), 'f', 1));
    } else {
        m_pendingStatus = QStringLiteral("大头：%1 · %2")
                              .arg(spot.names.join(QStringLiteral(" › ")),
                                   diskscan::formatBytes(leafBytes));
    }

    // startScan 会先把旧的大头条收起来，所以展示放在它之后
    startScan(target);
    showHotspot(spot);
}

void DiskScanPanel::showHotspot(const diskscan::Hotspot &spot)
{
    if (!m_hotBanner || spot.isEmpty())
        return;

    QString chain = QStringLiteral("占用大头：");
    for (int i = 0; i < spot.names.size(); ++i) {
        if (i > 0)
            chain += QStringLiteral(" › ");
        chain += spot.names.at(i);
    }
    chain += QStringLiteral(" · %1").arg(diskscan::formatBytes(spot.sizes.last()));
    if (spot.fileCounts.last() > 0)
        chain += QStringLiteral(" · %1 个文件").arg(diskscan::formatCount(spot.fileCounts.last()));
    if (spot.rootBytes > 0 && spot.sizes.last() < spot.rootBytes) {
        const QString rootName = QFileInfo(spot.root).fileName();
        chain += QStringLiteral("（占 %1 的 %2%）")
                     .arg(rootName.isEmpty() ? displayPath(spot.root) : rootName,
                          QString::number(100.0 * double(spot.sizes.last()) / double(spot.rootBytes), 'f', 1));
    }

    QString hint = spot.hint.text;
    if (hint.isEmpty()) {
        // 没命中已知类型时给一句"下一步能干什么"，而不是留空让用户猜
        hint = spot.isDir
                   ? QStringLiteral("没有匹配到已知的目录类型；可以进去看子项，勾选后移到回收站")
                   : QStringLiteral("单个文件就占了这么多，确认不用了再移到回收站");
    }
    if (spot.unreadableDirs > 0) {
        hint += QStringLiteral("；另有 %1 个子目录没有读取权限，体积只是下限")
                    .arg(spot.unreadableDirs);
    }

    m_hotChain->setText(chain);
    m_hotChain->setToolTip(displayPath(spot.leafPath()));
    m_hotHint->setText(hint);
    m_hotBanner->setVisible(true);
}

void DiskScanPanel::selectPath(const QString &path)
{
    if (path.isEmpty())
        return;
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
        QTreeWidgetItem *row = m_table->topLevelItem(i);
        if (row->data(ColName, kPathRole).toString() != path)
            continue;
        m_table->clearSelection();
        row->setSelected(true);
        m_table->setCurrentItem(row);
        m_table->scrollToItem(row, QAbstractItemView::PositionAtCenter);
        return;
    }
}

void DiskScanPanel::copySelectedPath()
{
    const diskscan::Item *item = selectedItem();
    if (!item)
        return;
    if (QGuiApplication::clipboard())
        QGuiApplication::clipboard()->setText(displayPath(item->path));
    setStatus(QStringLiteral("已复制路径：%1").arg(displayPath(item->path)), false);
}

void DiskScanPanel::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *row = m_table->itemAt(pos);
    if (!row)
        return;

    const diskscan::Item *item = itemByPath(row->data(ColName, kPathRole).toString());
    if (!item)
        return;

    QMenu menu(this);
    QAction *enter = menu.addAction(item->isDir ? QStringLiteral("进入这个文件夹")
                                                : QStringLiteral("打开这个文件"));
    enter->setEnabled(!m_scanning);
    QAction *reveal = menu.addAction(QStringLiteral("在文件管理器中打开"));
    menu.addSeparator();
    QAction *copy = menu.addAction(QStringLiteral("复制路径"));
    menu.addSeparator();
    QAction *trash = menu.addAction(QStringLiteral("移到回收站"));
    trash->setEnabled(!m_scanning && diskscan::trashBlockReason(item->path).isEmpty());

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == enter)
        activateItem(row);
    else if (chosen == reveal)
        revealSelected();
    else if (chosen == copy)
        copySelectedPath();
    else if (chosen == trash)
        trashSelected();
}

void DiskScanPanel::applySortColumn(int column)
{
    // 「占比」与「大小」是同一份数据的两种画法，点哪列都按大小排
    const int key = column == ColShare ? ColSize : column;
    if (key != ColName && key != ColSize && key != ColFiles && key != ColLevel)
        return;

    if (key == m_sortColumn) {
        m_sortDescending = !m_sortDescending;
    } else {
        m_sortColumn = key;
        // 名称从小到大最自然；体积与文件数先看最大的；等级按"能删的在前"
        m_sortDescending = key == ColSize || key == ColFiles;
    }
    m_table->header()->setSortIndicator(column, m_sortDescending ? Qt::DescendingOrder
                                                                 : Qt::AscendingOrder);
    rebuildTable();
    updateSelectionState();
}

const diskscan::Item *DiskScanPanel::selectedItem() const
{
    QTreeWidgetItem *row = m_table->currentItem();
    if (!row || !row->isSelected())
        return nullptr;
    return itemByPath(row->data(ColName, kPathRole).toString());
}

const diskscan::Item *DiskScanPanel::itemByPath(const QString &path) const
{
    if (path.isEmpty())
        return nullptr;
    const auto it = std::find_if(m_items.cbegin(), m_items.cend(),
                                 [&path](const diskscan::Item &item) { return item.path == path; });
    return it == m_items.cend() ? nullptr : &(*it);
}

void DiskScanPanel::setBusy(bool busy)
{
    m_volume->setEnabled(!busy);
    m_up->setEnabled(!busy && !m_root.isEmpty() && !QDir(m_root).isRoot());
    m_path->setEnabled(!busy);
    m_browse->setEnabled(!busy);
    m_scan->setEnabled(!busy);
    m_stop->setEnabled(busy);
    m_hotspot->setEnabled(!busy);
    // 筛选不锁：扫描途中也可以勾选，rebuildTable 只读 m_items，不会和 worker 抢数据
    if (busy) {
        m_reveal->setEnabled(false);
        m_trash->setEnabled(false);
        m_cleanAll->setVisible(false);   // 扫描中列表在半成品状态，不适合一键清
    }
}

void DiskScanPanel::setStatus(const QString &text, bool warn)
{
    m_status->setText(text);
    // 状态栏会被窗口宽度截断，完整内容挂到 tooltip 上，避免"有用信息被省略号吃掉"
    m_status->setToolTip(text);
    const QString state = warn ? QStringLiteral("warn") : QStringLiteral("normal");
    if (m_status->property("state").toString() != state) {
        m_status->setProperty("state", state);
        repolish(m_status);
    }
}

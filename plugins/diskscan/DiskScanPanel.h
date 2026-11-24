#pragma once

// ---------------------------------------------------------------------------
// 磁盘空间面板（插件内部使用，不进 api/，宿主只认 ITool）。
//
// 从上到下六块：
//   1. 概览卡：当前磁盘的占用百分比 + 已用/总量 + 剩余空间；
//   2. 操作条：目标磁盘下拉、上一级、路径输入框（可粘贴）、浏览、扫描、停止；
//   3. 扫描进度条（只在扫描时占位）；
//   4. 面包屑：点任意一段直接跳到那一层；
//   5. 筛选条：只看建议清理的项 / 隐藏小于 50 MB + 一行汇总 + 一键清掉可清理项；
//   6. 大头条（只有按「找大头」之后才出现）：把"空间被哪一条链路吃掉"摆出来；
//   7. 明细表：名称 / 大小 / 占比（比例条）/ 文件数 / 说明 / 建议，底部为状态与操作。
//
// 扫描一定放工作线程：一个几百 GB 的盘要遍历上百万个文件，在 UI 线程里跑会直接假死。
// 每算完一个子项就把结果排队送回 UI 线程（QMetaObject::invokeMethod，context 是 this），
// 所以用户能看着列表一项项长出来，而不是盯着转圈等最后一下。
//
// 清理只做「移到回收站」（QFile::moveToTrash），**不做永久删除**：
// 误删一个 200 GB 的目录无法挽回，回收站还能还原。系统目录 / 盘根 / 用户资料根目录
// 由 DiskScan.h 的 classify() 判成 Protected，界面上不给清理入口。
//
// 状态全部是实例成员：宿主允许同一工具同时存在「内嵌」与「独立窗口」两份面板，
// 用静态变量会互相串状态。
// ---------------------------------------------------------------------------

#include "DiskScan.h"

#include <QVector>
#include <QWidget>

#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QThread;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class DiskScanPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DiskScanPanel(QWidget *parent = nullptr);
    ~DiskScanPanel() override;

protected:
    /// 第一次显示就开始扫主目录：工具的全部价值都在那张表上，空表进去没意义
    void showEvent(QShowEvent *event) override;

    /// 下拉框右侧的小三角是自绘子控件，得跟着下拉框的尺寸与可用状态走
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// 枚举挂载的磁盘，填进下拉框（主目录固定排第一）
    void reloadVolumes();

    /// 起工作线程扫描 path 的直接子项。扫描中重复调用会被忽略（先按「停止」）
    void startScan(const QString &path);

    /// 请求中断当前扫描：worker 每 256 个文件检查一次标志，通常百毫秒内退出
    void cancelScan();

    /// worker 每算完一个子项就把结果投递过来，这里追加并触发节流刷新
    void appendItem(const diskscan::Item &item);

    /// worker 退出后回到 UI 线程：收尾、写状态栏、恢复按钮
    void applyScanFinished(const diskscan::Result &result);

    /// 按当前排序列与筛选条件把 m_items 重建成表格行（保留选中项与滚动位置）
    void rebuildTable();

    /// 面包屑：把 m_root 逐段拆成可点击的按钮，最后一段是当前位置
    void rebuildCrumbs();

    /// 概览卡：当前路径所在磁盘的占用情况
    void updateSummary();

    /// 筛选条右侧那一行"共 N 项 · 合计 X · 建议清理 M 项约 Y"
    void updateFilterSummary();

    void updateSelectionState();

    /// 上一级（不越过盘根）
    void goUp();

    /// 双击 / 回车：文件夹进入下一层，文件用系统默认程序打开
    void activateItem(QTreeWidgetItem *item);

    /// 在系统文件管理器里打开选中项
    void revealSelected();

    /// 二次确认后把选中项移到回收站，然后重新统计当前目录
    void trashSelected();

    /// 二次确认后把一组数据项移到回收站（选中项 / 全部可清理项共用）
    void trashItems(const QVector<diskscan::Item> &items);

    /// 把列表里标了「可清理」的项一次清掉（列表本身受筛选影响，所见即所清）
    void cleanCleanable();

    /// 从当前目录一路跟进最大的子项，找出"大头"并直接导航过去
    void findHotspot();

    /// worker 退出后回到 UI 线程：展示大头条 + 跳到那一层
    void applyHotspotFinished(const diskscan::Hotspot &spot);

    /// 把热点链路写进大头条（扫描结束后仍保留，直到用户换位置）
    void showHotspot(const diskscan::Hotspot &spot);

    /// 扫描完成后按路径选中并滚动到某一行（找大头落地后高亮那一项）
    void selectPath(const QString &path);

    void copySelectedPath();

    void showContextMenu(const QPoint &pos);

    /// 表头点击 → 换排序列 / 翻转升降序
    void applySortColumn(int column);

    /// 当前选中行对应的数据项；没有选中或已失效返回 nullptr
    const diskscan::Item *selectedItem() const;

    /// 按路径找数据项（选中态、右键菜单共用）
    const diskscan::Item *itemByPath(const QString &path) const;

    /// 把下拉框切到包含 path 的那块磁盘（找不到就保持原样，不误报）
    void syncVolumeTo(const QString &path);

    void setBusy(bool busy);
    void setStatus(const QString &text, bool warn);

    /// 把"清理完成"这类挂起文案吐到状态栏（扫描收尾时调用，见 m_pendingStatus）
    void applyPendingStatus();

    // ---- 概览卡
    QLabel *m_percent = nullptr;
    QLabel *m_used = nullptr;
    QLabel *m_avail = nullptr;
    QProgressBar *m_diskBar = nullptr;

    // ---- 操作条
    QComboBox *m_volume = nullptr;
    QWidget *m_comboArrow = nullptr;
    QPushButton *m_up = nullptr;
    QLineEdit *m_path = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_scan = nullptr;
    QPushButton *m_stop = nullptr;
    QProgressBar *m_scanBar = nullptr;
    QPushButton *m_hotspot = nullptr;   ///< 找大头

    // ---- 面包屑与筛选
    QHBoxLayout *m_crumbLayout = nullptr;
    QCheckBox *m_onlyCleanable = nullptr;
    QCheckBox *m_hideSmall = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_cleanAll = nullptr;   ///< 一键清掉列表里标了可清理的项

    // ---- 大头条
    QFrame *m_hotBanner = nullptr;
    QLabel *m_hotChain = nullptr;
    QLabel *m_hotHint = nullptr;

    // ---- 明细表与底部
    QStackedWidget *m_stack = nullptr;
    QLabel *m_empty = nullptr;
    QTreeWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_reveal = nullptr;
    QPushButton *m_trash = nullptr;

    QTimer *m_flushTimer = nullptr;   ///< 扫描中的节流刷新（120ms）
    QThread *m_thread = nullptr;
    QThread *m_hotThread = nullptr;   ///< 找大头的工作线程
    std::shared_ptr<std::atomic<bool>> m_cancel;   ///< worker 与 UI 线程共享的取消标志

    QVector<diskscan::Item> m_items;
    QString m_root;          ///< 当前扫描的目录（界面上的"位置"）
    int m_done = 0;
    int m_total = 0;
    bool m_scanning = false;
    bool m_hasScanned = false;   ///< showEvent 只在第一次自动扫描，独立窗口来回搬不重扫
    int m_sortColumn = 1;        ///< 默认按「大小」列排
    bool m_sortDescending = true;
    int m_lastLevel = -1;        ///< 概览卡警戒等级，只在变化时重刷样式
    /// 当前**可见**行里可清理的项数与体积：一键清理只认这个数（所见即所清）
    int m_visibleCleanable = 0;
    quint64 m_visibleCleanableBytes = 0;

    /// 清理等操作的结果文案：先记下来，等"清理后重新统计"这一轮扫描结束再显示。
    /// 直接写状态栏会被 startScan 的"正在统计…"和随后的进度回调立刻盖掉。
    QString m_pendingStatus;
    bool m_pendingWarn = false;

    /// 扫描结束后要选中的行（找大头跳到某一层时用；空表示不选）
    QString m_pendingSelectPath;
};

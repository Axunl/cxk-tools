#pragma once

// ---------------------------------------------------------------------------
// 内存监控面板（插件内部使用，不进 api/，宿主只认 ITool）。
//
// 三块内容，从上到下：
//   1. 概览卡：物理内存占用百分比 + 已用/总量 + 进度条 + 可用 + 本次已释放累计；
//   2. 操作条：进程搜索、自动释放开关与阈值、释放选中 / 释放内存；
//   3. 进程表：按工作集从大到小，右键可单独释放某个进程。
//
// 面板**只在可见时**采样（showEvent 起定时器、hideEvent 停），否则藏在页栈里的
// 面板也会 2 秒一次去扫进程表，白烧 CPU。
//
// 状态全部留在实例里（自动释放开关、已释放累计、阈值选择），没有静态变量：
// 宿主允许同一工具同时存在「主窗口内嵌」与「独立窗口」两份面板，用全局状态会互相串。
// ---------------------------------------------------------------------------

#include "MemProbe.h"

#include <QDateTime>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class MemMonPanel : public QWidget
{
    Q_OBJECT

public:
    explicit MemMonPanel(QWidget *parent = nullptr);

protected:
    /// 面板可见才开始采样；顺带把焦点丢进搜索框（进插件就能直接筛进程）
    void showEvent(QShowEvent *event) override;

    /// 面板不可见就停掉定时器：内嵌面板被页栈藏起来后还在扫进程纯属浪费
    void hideEvent(QHideEvent *event) override;

    /// 阈值下拉框的箭头是自绘子控件，得跟着下拉框的尺寸与可用状态走
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// 采样一次（系统内存 + 进程表），刷新概览与列表
    void refresh();

    /// 只按当前关键词与缓存重排列表（搜索框输入时用，不重新扫进程）
    void rebuildTable();

    /// 整行重建：结构变了才走这里，会保下选中行与滚动位置
    void rebuildTableFully(const QVector<memprobe::ProcessMemory> &rows);

    /// 就地更新某一行的文字（行数与顺序都没变时走这里，避免每 2 秒闪一下）
    void updateRow(QTreeWidgetItem *item, const memprobe::ProcessMemory &process);

    void updateSummary();

    /// 回收一批进程。@param automatic 决定文案是"自动释放"还是手动结果
    void releaseProcesses(const QVector<quint32> &pids, bool automatic);

    void releaseSelected();
    void releaseAll();

    /// 定时器回调里判断"要不要自动释放"：低于阈值 + 冷却期已过才动手
    void maybeAutoRelease();

    void updateSelectionState();
    quint32 selectedPid() const;
    void showContextMenu(const QPoint &pos);

    /// @param holdSecs > 0 表示这条消息先占住状态栏若干秒，别被下一次刷新覆盖
    void setStatus(const QString &text, bool warn, int holdSecs);

    // ---- 概览卡
    QLabel *m_percent = nullptr;
    QLabel *m_used = nullptr;
    QLabel *m_avail = nullptr;
    QLabel *m_released = nullptr;
    QProgressBar *m_bar = nullptr;

    // ---- 操作条与列表
    QLineEdit *m_search = nullptr;
    QCheckBox *m_auto = nullptr;
    QComboBox *m_threshold = nullptr;
    QWidget *m_comboArrow = nullptr;        ///< 阈值框右侧的自绘小三角（qss 画不好，见实现）
    QPushButton *m_releaseSelected = nullptr;
    QPushButton *m_releaseAll = nullptr;
    QTreeWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_timer = nullptr;

    // ---- 数据与状态
    memprobe::SystemMemory m_sys;
    QVector<memprobe::ProcessMemory> m_processes;
    QVector<quint32> m_rowPids;        ///< 当前表格里每一行的 pid，用于判断"结构是否变了"
    quint64 m_releasedTotal = 0;       ///< 本次面板生命周期内累计回收（面板关掉即清零）
    QDateTime m_lastAutoRelease;       ///< 上次自动释放时间，用于冷却
    QDateTime m_statusHoldUntil;       ///< 状态栏消息的占用截止时间
    bool m_releasing = false;          ///< 正在回收，防止 refresh → 自动释放 → refresh 递归
    int m_lastLevel = -1;              ///< 概览卡当前警戒等级，只在变化时重刷样式
};

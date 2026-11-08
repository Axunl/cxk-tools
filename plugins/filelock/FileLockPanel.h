#pragma once

// ---------------------------------------------------------------------------
// 文件占用释放面板（插件内部使用，宿主只认 ITool）。
//
// 从上到下四块，对齐参考界面：
//   1. 路径行：多路径输入框 + 浏览 + 「递归查询」复选框；
//   2. 拖拽区：接收文件 / 文件夹，多个目标用分号拼进输入框；
//   3. 操作行：检测占用（主操作）/ 解除占用 / 强制结束；
//   4. 结果表（程序 / PID / 类型 / 状态 / 操作）+ 底部状态栏。
//
// 检测放工作线程：Windows 的 Restart Manager 要注册最多 512 个文件，
// Linux 要扫全部进程的 fd，都可能超过一秒，放在 UI 线程会明显假死。
// 面板在检测途中被销毁时，工作线程只写一份共享结果，finished 的回调以 this
// 为 context（对象销毁后 Qt 会丢弃该次投递），不会回调到已释放的面板上。
//
// 所有状态都是实例成员：宿主允许同一工具同时存在「内嵌」与「独立窗口」两份面板。
// ---------------------------------------------------------------------------

#include "FileLockLogic.h"

#include <QVector>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTableWidget;
class QThread;

class FileLockPanel : public QWidget
{
    Q_OBJECT

public:
    explicit FileLockPanel(QWidget *parent = nullptr);
    ~FileLockPanel() override;

protected:
    /// 进插件就能直接粘贴 / 输入路径；同步设焦点会被宿主的焦点链覆盖
    void showEvent(QShowEvent *event) override;

private:
    /// 「浏览...」按钮弹菜单，分别选文件与文件夹（QFileDialog 两种模式不能共存）
    void browsePath();

    /// 拖入的路径追加到输入框，而不是覆盖用户已经填好的内容
    void appendDroppedPaths(const QStringList &paths);

    /// 起工作线程做一次检测；重复点击由 m_scanning 挡住
    void startScan();

    /// 工作线程结束后回到 UI 线程：重建表格、写状态栏
    void applyScanResult(const filelock::ScanOutcome &outcome);

    void rebuildTable();

    /// 勾选行数 → 底部「已选择 N 个程序」与三个按钮的可用性
    void updateSelectionState();

    QVector<quint64> selectedPids() const;

    /// 二次确认后关闭所选进程里指向目标文件的句柄（平台不支持会显示原因）
    void releaseSelected();

    void terminateSelected();
    void terminateOne(quint64 pid);

    /// 统一的结束流程：确认 → 执行 → 改状态列。单个行按钮与批量按钮共用
    void terminatePids(const QVector<quint64> &pids);

    /// 把成功处理过的行标成「已解除 / 已结束」并取消勾选
    void markRows(const QVector<quint64> &pids, const QString &status);

    void clearAll();

    /// @param warn 红色告警，@param busy 蓝色进行中，两者都否是绿色就绪
    void setStatus(const QString &text, bool warn, bool busy);

    quint64 selfPid() const;

    QLineEdit *m_path = nullptr;
    QCheckBox *m_recursive = nullptr;
    QPushButton *m_browse = nullptr;
    QPushButton *m_detect = nullptr;
    QPushButton *m_release = nullptr;
    QPushButton *m_terminate = nullptr;

    QStackedWidget *m_stack = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_empty = nullptr;

    QLabel *m_dot = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_selected = nullptr;
    QPushButton *m_clear = nullptr;

    QThread *m_scanThread = nullptr;
    QVector<filelock::Holder> m_holders;
    bool m_scanning = false;   ///< 检测进行中
    bool m_building = false;   ///< 重建表格期间吞掉 itemChanged，避免误刷选中数
};

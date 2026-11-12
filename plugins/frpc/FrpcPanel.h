#pragma once

// ---------------------------------------------------------------------------
// frpc 内网穿透面板。
//
// 三块内容，从上到下：
//   1. 工具条：配置档下拉（新建 / 复制 / 重命名 / 删除）+ frpc 可执行文件路径；
//   2. 状态卡：运行状态胶囊、服务端地址、已运行时长、日志条数，右侧「探活」「启动/停止」；
//   3. 分栏：上面是配置编辑区（实时校验，右边显示结论），下面是运行日志（按级别着色）。
//
// 与宿主的约定（docs/PLUGIN.md）：
//   * 面板每次进入都新建，所以「上次选哪份配置、配置内容、frpc 路径」全部落盘
//     （FrpcStore），下次进来恢复现场；
//   * 进程不在面板里，见 FrpcRunner.h 的 Service —— 切页 / 回首页都不会中断穿透；
//   * 所有颜色走 applyStyle + @令牌，深色主题下不重启也能跟着换。
//
// 面板自己的状态（当前选中的档、未保存的编辑内容、日志显示游标）都是实例成员，
// 因为宿主可能同时开「主窗口内嵌」与「独立窗口」两份面板。
// ---------------------------------------------------------------------------

#include "FrpcStore.h"
#include "FrpcText.h"
#include "ThemePalette.h"   // setHint() 的参数用到颜色令牌，头文件里必须可见

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTextEdit;
class QTimer;
class QTcpSocket;

class FrpcPanel : public QWidget
{
    Q_OBJECT

public:
    explicit FrpcPanel(QWidget *parent = nullptr);
    ~FrpcPanel() override;

protected:
    /// 面板可见才开始刷新（状态、日志、运行时长）。藏起来时定时器停掉：
    /// 内嵌面板被页栈藏起来后还每 600ms 读一次日志纯属浪费。
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    /// 宿主换肤时 Qt 未必发 ThemeChange（宿主是自己替换 qss 的），
    /// 所以这里额外在每次重绘时比对深色标志，变了就重画日志区。
    void changeEvent(QEvent *event) override;

private:
    // ---- 构建
    QWidget *buildToolbar();
    QWidget *buildStatusCard();
    QWidget *buildConfigArea();
    QWidget *buildLogArea();

    // ---- 配置档
    void reloadProfileCombo(int selectIndex);
    void loadProfile(int index);
    void saveCurrent(bool immediate);
    void createProfile();
    void duplicateProfile();
    void renameProfile();
    void deleteProfile();
    frpc::Profile *currentProfile();

    // ---- frpc 路径
    void browseExecutable();
    void detectExecutable();

    // ---- 运行
    void startFrpc();
    void stopFrpc();
    void toggleRun();
    void refreshState();
    void onServiceStateChanged();

    // ---- 配置校验
    void refreshValidation();

    // ---- 日志
    void pumpLog();
    void appendLogView(const QStringList &lines);
    void rebuildLogView();
    void trimLogView();
    void clearLogView();
    void copyLog();

    // ---- 探活
    void probeServer();

    // ---- 提示
    void setHint(const QString &text, xtools::theme::Color token);
    void setIdleHint();

    // ---- 工具条控件
    QComboBox *m_profileCombo = nullptr;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_renameButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QLineEdit *m_binPath = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_detectButton = nullptr;

    // ---- 状态卡控件
    QLabel *m_statePill = nullptr;
    QLabel *m_endpointLabel = nullptr;
    QLabel *m_uptimeLabel = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_probeButton = nullptr;
    QPushButton *m_runButton = nullptr;

    // ---- 编辑与日志
    QLabel *m_formatLabel = nullptr;
    QLabel *m_validationLabel = nullptr;
    QPlainTextEdit *m_configEdit = nullptr;
    QTextEdit *m_logView = nullptr;
    QSplitter *m_splitter = nullptr;

    // ---- 状态
    frpc::Store m_store;
    QVector<frpc::Profile> m_profiles;
    int m_currentIndex = -1;
    QTimer *m_tickTimer = nullptr;      ///< 600ms：刷新状态、日志、运行时长
    QTimer *m_saveTimer = nullptr;      ///< 700ms：编辑防抖保存
    QTimer *m_validateTimer = nullptr;  ///< 300ms：编辑防抖校验
    QTcpSocket *m_probeSocket = nullptr;

    /// 日志显示游标：Service 的绝对行号，下次从这一行开始要。
    qint64 m_logCursor = 0;
    /// 日志区已显示的 (级别, 文本)，换肤时整块重画用。
    struct LogEntry
    {
        frpctext::LogKind kind;
        QString text;
    };
    QVector<LogEntry> m_logLines;
    /// 上次绘制日志时是不是深色主题（changeEvent 里比对用）。
    bool m_renderedDark = false;
    /// 上一次刷新的服务状态，用来发现「刚启动就退出」这种异常。
    bool m_wasRunning = false;
    qint64 m_lastStartMs = 0;
    /// 已经请求过停止的时刻，用于区分「用户停的」和「自己崩的」。
    qint64 m_stopRequestedAtMs = 0;
};

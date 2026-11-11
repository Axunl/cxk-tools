#pragma once

// ---------------------------------------------------------------------------
// 屏幕录制面板。**只是 RecordService 的视图**，不持有任何录制状态。
//
// 这条边界是刻意划的：宿主会在回首页 / 切工具 / 关窗口时 deleteLater() 掉面板
// （MainWindow::destroyToolPanel），如果录制状态跟着面板走，用户点一下首页就
// 把正在录的视频截断了。所以：
//   * 面板只订阅 service 的信号、按 service 的状态渲染；
//   * 析构函数什么都不做（不 stop、不 finish）；
//   * 构造时读一次 service 状态即可恢复「录制中 00:23」，因为 applyState 是幂等只读。
//
// 同一时刻可能存在两份面板（内嵌页 + 独立窗口），两份都能点停止，第二次点自动 no-op。
// ---------------------------------------------------------------------------

#include "RecordService.h"
#include "ThemePalette.h"

#include <QHash>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QProcess;
class QPushButton;
class QTimer;

namespace screenrec {
class RegionSelectOverlay;
}

class ScreenRecPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenRecPanel(QWidget *parent = nullptr);

    /// 刻意空实现：面板销毁不停止录制（见文件头）。
    ~ScreenRecPanel() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *buildSetupCard();
    QWidget *buildLiveCard();

    void applyState(screenrec::RecordService::State state);
    void refreshLive();
    void refreshRegionInfo();
    void refreshScreenList();

    void onStartStop();
    void onPickRegion();
    void onChooseDir();
    void onOpenDir();
    void onOpenFile();
    void onConvert();
    void onStateChanged(screenrec::RecordService::State state);
    void onFinished(const QString &path);
    void onFailed(const QString &message);

    void setStatus(const QString &text, xtools::theme::Color token);
    /// 主按钮的三种外观（正常 / 危险 / 禁用）。改完动态属性必须 repolish，
    /// 否则 qss 里 [accent="true"] 这类规则不会立刻生效。
    void restylePrimaryButton(const QString &text, bool accent, bool danger, bool enabled);

    screenrec::RecordService::Config buildConfig() const;
    QScreen *selectedScreen() const;
    bool screenIsLarge() const;
    void applyDefaultFpsForScreen();

    screenrec::RecordService *m_service = nullptr;

    QFrame *m_setupCard = nullptr;
    QFrame *m_liveCard = nullptr;

    QComboBox *m_screenBox = nullptr;
    QComboBox *m_fpsBox = nullptr;
    QComboBox *m_qualityBox = nullptr;
    QComboBox *m_modeBox = nullptr;
    QPushButton *m_pickRegion = nullptr;
    QLabel *m_regionInfo = nullptr;
    QCheckBox *m_cursorBox = nullptr;
    QLabel *m_outDirLabel = nullptr;
    QPushButton *m_chooseDir = nullptr;
    QPushButton *m_openDir = nullptr;

    QLabel *m_time = nullptr;
    QLabel *m_stateText = nullptr;
    QLabel *m_detail = nullptr;

    QPushButton *m_startStop = nullptr;
    QPushButton *m_convert = nullptr;
    QPushButton *m_openFile = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_hint = nullptr;

    /// 下拉框右侧的自绘小三角。全局 qss 去掉了原生箭头，必须自己补。
    QHash<QComboBox *, QWidget *> m_arrows;

    QTimer *m_liveTimer = nullptr;
    QPointer<screenrec::RegionSelectOverlay> m_regionOverlay;
    QProcess *m_convertProcess = nullptr;

    QString m_outputDir;
    QRect m_regionLogic;      ///< 逻辑像素，相对 m_regionScreen 的左上角
    QString m_regionScreen;
    QString m_lastFile;
    bool m_hasFfmpeg = false;
};

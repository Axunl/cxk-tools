#pragma once

// ---------------------------------------------------------------------------
// 「发现新版本」对话框：版本对比、发布说明、下载进度、安装动作。
//
// 它只负责流程编排与文案，网络与校验都在 UpdateChecker，退出程序由 AppController
// 负责（dialog 通过 installerLaunched() 把决定权交回去）。
//
// 为什么不做成嵌在设置页里的一块：更新是低频但需要全神贯注的动作，
// 一屏之内要同时展示版本差异、发布说明和进度，塞进设置页会把那页挤爆。
// ---------------------------------------------------------------------------

#include "UpdateInfo.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QString>

class UpdateChecker;
class Settings;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;

class UpdateDialog : public QDialog
{
    Q_OBJECT

public:
    UpdateDialog(UpdateChecker *checker, Settings *settings, QWidget *parent = nullptr);

    /// 展示某个新版本。重复调用只更新内容（同一个窗口复用）。
    void showUpdate(const update::Info &info);

signals:
    /// 已经拉起安装器，主程序应当马上退出（安装器收尾时会把新版本重新启动）。
    void installerLaunched();

protected:
    /// 关窗等价于「稍后」：记住这个版本号，静默检查不再为它弹提示。
    void closeEvent(QCloseEvent *event) override;

private:
    void onPrimaryClicked();
    void openReleasePage();
    void rememberSkip();
    void setStatus(const QString &text, bool error = false);
    void updateButtons();

    /// 把「已下载 / 总量 · 速度 · 剩余」写成一行并同步进度条。
    ///
    /// 为什么不能只显示百分比：安装包 29MB，在只跑得动 60KB/s 的线路上要八分钟，
    /// 头一分钟一直停在 3%，用户根本分不清"在下"和"卡死"——这正是
    /// 「检查更新每次都更新不了」的观感来源（2026-11-15 实测）。
    void updateProgressText(qint64 received, qint64 total);
    /// 重新打开对话框时接上 UpdateChecker 里可能还在跑的下载，返回是否接上了。
    bool attachRunningDownload();
    /// 测速窗口清零：新下载（或刚接上一段旧下载）时调用，别把两个包的时间混在一起。
    void resetSpeedWindow();

    UpdateChecker *m_checker = nullptr;
    Settings *m_settings = nullptr;

    update::Info m_info;
    const update::Asset *m_asset = nullptr;   ///< 指向 m_info.assets 里的元素，只在本次界面生命周期内有效
    QString m_assetKind;                      ///< 期望的包类型：安装版用 installer，便携版用 portable

    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QTextBrowser *m_notes = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_primary = nullptr;
    QPushButton *m_pageButton = nullptr;
    QPushButton *m_laterButton = nullptr;

    bool m_downloading = false;

    QElapsedTimer m_speedClock;   ///< 测速窗口的起点；无效表示还没收到第一个进度事件
    qint64 m_speedBytes = 0;      ///< 窗口起点时已下载的字节数
    double m_speed = 0.0;         ///< 平滑后的速度（字节/秒），0 表示还算不出来
};

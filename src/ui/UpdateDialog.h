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
};

#pragma once

// ---------------------------------------------------------------------------
// 更新检查器（core 层，不认识任何 QWidget）。
//
// 数据源：GitHub Releases 上的 latest.json（地址见 feedUrl()），不打 GitHub API，
// 因此没有 60 次/小时的匿名限流，也不会因为仓库改名之外的原因中途失效。
//
// 分层约定：
//   UpdateInfo.h   纯逻辑（比版本、解析清单），可离线断言
//   UpdateChecker  网络 + 落盘 + 校验（本文件）
//   UpdateDialog   界面与「下载/安装」按钮的编排
//   AppController  把上面三者接起来，并负责退出自己
//
// 失败语义：所有错误都通过 failed/downloadFailed 抛出可读中文，绝不静默失败
// （AGENTS 规则 3：平台能力缺失也要给明确文案）。
// ---------------------------------------------------------------------------

#include "UpdateInfo.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QNetworkReply;

class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    /// 用户主动触发的检查（设置页按钮 / 托盘菜单）。
    void check();

    /// 启动时的静默检查：延迟 3 秒、24 小时内不重复、失败只写日志。
    /// 为什么延迟：启动瞬间要先让窗口出来，网络请求和首次绘制抢同一条线程没有意义。
    void scheduleStartupCheck();

    /// 下载清单里的某一项到临时目录。已在校验 SHA256 后通过 downloadReady 通知。
    void download(const update::Asset &asset);

    /// 更新源。可用 XTOOLS_UPDATE_FEED 覆盖（本地验证时指向 file:// 的清单即可）。
    static QUrl feedUrl();

    /// 当前这份可执行文件是不是「安装版」（同目录有 Inno Setup 的 unins000.exe）。
    /// 便携版不做静默升级：那会把新版本装进 Program Files，而用户手里还是旧目录，
    /// 下次打开依旧是旧的，属于"看起来成功了其实没更新"。
    static bool isInstalledBuild();

    /// 静默运行安装器（Windows）。成功后调用方应当立即退出自己，
    /// 安装器收尾时会把新版本重新拉起来（见 .iss 的 [Run] 段）。
    static bool launchInstaller(const QString &filePath, QString *error);

signals:
    void checking();
    void updateAvailable(const update::Info &info);
    void upToDate(const QString &currentVersion);
    void failed(const QString &message);

    void downloadProgress(qint64 received, qint64 total);
    void downloadReady(const QString &filePath, const update::Asset &asset);
    void downloadFailed(const QString &message);

private:
    void requestFeed();
    void handleFeedFinished(QNetworkReply *reply);
    void finishDownload(bool ok, const QString &error);

    QNetworkAccessManager m_nam;
    QNetworkReply *m_feedReply = nullptr;
    /// 本次请求是不是「启动静默检查」：决定发现新版本时要不要弹气泡、
    /// 以及要不要尊重用户点过的「稍后」。
    bool m_silent = false;

    QNetworkReply *m_downloadReply = nullptr;
    QFile m_downloadFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};   ///< 边下边算，收完直接比
    qint64 m_received = 0;
    qint64 m_total = 0;
    update::Asset m_downloadAsset;
};

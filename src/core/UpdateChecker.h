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
//
// 两条与「更新老是失败」直接相关的设计（2026-11 修）：
//   1) Qt 把传输超时统一报成 QNetworkReply::OperationCanceledError，errorString 只有
//      "Operation canceled"。原样丢给界面，用户只知道失败、不知道是网络卡住了，
//      所以必须把错误码翻成人话（见 describeNetworkError）。
//   2) GitHub 的下载域名在境内经常几十 KB/s 甚至断流，一次性下 30MB 只要中途抖一下
//      就全废。所以下载改成「.part 落盘 + Range 断点续传 + 自动重连」。
// ---------------------------------------------------------------------------

#include "UpdateInfo.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

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
    /// 断流会自动重连并从断点续传；彻底失败时 downloadFailed 给的是中文原因。
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
    /// 断流后正在等下一轮重连。界面据此把"卡住了"解释给用户，而不是干等进度条不动。
    void downloadRetrying(int attempt, int maxAttempts, const QString &reason);
    void downloadReady(const QString &filePath, const update::Asset &asset);
    void downloadFailed(const QString &message);

private:
    void requestFeed();
    /// 发一次清单请求。generation 用来识别"这次请求是否已经过期"（用户连点检查、
    /// 或重试排队期间又发起新请求时，旧回调要自己认命退出）。
    void fireFeedRequest(int generation);
    void handleFeedFinished(QNetworkReply *reply, int generation);

    void startAttempt();
    void handleDownloadFinished(QNetworkReply *reply);
    void verifyDownload();
    void scheduleRetry(const QString &reason);
    void failDownload(const QString &reason, bool keepPartial);
    /// 把 Qt 的网络错误码翻成能指导用户下一步的中文。
    QString describeNetworkError(QNetworkReply::NetworkError code, const QString &raw) const;

    QNetworkAccessManager m_nam;
    QNetworkReply *m_feedReply = nullptr;
    /// 本次请求是不是「启动静默检查」：决定发现新版本时要不要弹气泡、
    /// 以及要不要尊重用户点过的「稍后」。
    bool m_silent = false;
    int m_feedGeneration = 0;
    int m_feedAttempt = 0;

    QNetworkReply *m_downloadReply = nullptr;
    QFile m_downloadFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};   ///< 边下边算，收完直接比
    qint64 m_received = 0;      ///< 已经落在磁盘上、并且已经进 hash 的字节数
    qint64 m_total = 0;
    qint64 m_attemptBase = 0;   ///< 本轮开始时的字节数，用于在缺 Content-Length 时推算总量
    int m_attempt = 0;          ///< 第几次尝试（1 起），到 kDownloadAttempts 就放弃
    /// 本轮用的传输超时（毫秒）。Qt 的超时错误只说 "Operation canceled"，
    /// 报错文案要能说出"等了多久"，所以得把它记下来。
    int m_currentTimeoutMs = 0;
    /// 上一轮是否一个字节都没收到：是的话本轮改用短超时，避免"没网"等上几分钟。
    bool m_useStalledTimeout = false;
    bool m_retryPending = false;///< 重连前的等待窗口里为 true，防止并发起第二条链路
    bool m_rangeChecked = false;///< 本轮响应是否已经判断过服务器认不认 Range
    QString m_localError;       ///< 写盘之类的本地故障：重试救不了，要单独报
    QString m_partPath;         ///< 下载中的临时文件（xxx.part），校验通过后才改名
    QString m_targetPath;       ///< 最终落盘路径
    update::Asset m_downloadAsset;
};

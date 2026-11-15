#pragma once

// ---------------------------------------------------------------------------
// 更新检查器（core 层，不认识任何 QWidget）。
//
// 数据源：GitHub Releases 上的 latest.json（地址见 feedUrl()），不打 GitHub API，
// 因此没有 60 次/小时的匿名限流，也不会因为仓库改名之外的原因中途失效。
//
// 代价是版本号完全取决于 GitHub 认为哪个 release 是「latest」——它按**发布时间**判定，
// 不是按版本号大小。重跑/补发一个旧 tag 会让旧版本抢到 latest，于是大家（包括自己）
// 永远只会被提示升级到那个旧版本。2026-11-15 就踩过一次：v0.10.0 比 v0.9.0 早发布
// 一分钟，结果 releases/latest 一直指向 v0.9.0。CI 侧已加保护（release.yml 发布后
// 显式把当前 tag 标成 latest，且不降级），本文件在清单版本比本机还旧时打日志便于定位。
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
//   3) 国内直连 GitHub 基本不可用，必须让更新走本机代理。代理只作用在本类的
//      QNetworkAccessManager 上（见 NetProxy），不装全局工厂——插件里的 HTTP 调试、
//      WebSocket 测试、局域网共享不该被"下更新要用代理"这件事牵连。
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

    /// 是否正有一次下载在跑（含断流后等待重连的那几秒）。
    ///
    /// 为什么必须暴露：下载跑在 UpdateChecker 上，对话框只是它的一个观察者。
    /// 用户关掉对话框再点「检查更新」时，对话框会重新构造一次状态——如果它
    /// 直接把自己当成"空闲"，按钮就变回「下载并安装」，点下去只会得到
    /// 「上一个下载还没结束」。用户看到的就是"更新永远失败"，而其实下载还在跑。
    bool isDownloading() const;

    /// 正在下载的文件名（没在下就返回空）。对话框据此判断「接上的是不是同一个包」。
    QString downloadingName() const;

    /// 已落盘字节数 / 清单里写的总字节数（0 表示清单没给 size）。
    /// 供对话框重开时把进度条直接画回当前位置，不用等下一次进度信号。
    qint64 receivedBytes() const { return m_received; }
    qint64 totalBytes() const { return m_total; }

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
    /// 按当前设置给这个地址挑代理并套到 m_nam 上（结果缓存在 NetProxy 里，
    /// 每次请求前调一次即可；设置页改完代理后 NetProxy::reload() 会让它重算）。
    void applyProxy(const QUrl &url);

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

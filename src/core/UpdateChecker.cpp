#include "UpdateChecker.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace {

// 更新源固定指向本仓库 release 上的 latest.json。
//
// 为什么不用 api.github.com/repos/.../releases/latest：匿名调用每小时只有 60 次，
// 而且一旦被限流，用户看到的是「检查更新失败」，很难解释。releases/latest/download/
// 是 CDN 重定向，直接拿文件，没有配额问题。
constexpr auto kFeedUrl = "https://github.com/Axunl/cxk-tools/releases/latest/download/latest.json";

/// 自动检查的节流窗口。一天一次足够，也不至于让用户觉得程序老在联网。
constexpr int kCheckIntervalHours = 24;
/// 启动后多久开始静默检查：先让窗口画出来，别和首帧抢时间。
constexpr int kStartupDelayMs = 3000;
/// 清单很小，10 秒足够；给太长会让「网络不通」的体验变成卡住不动。
constexpr int kFeedTimeoutMs = 10000;
/// 安装包有二三十兆，超时要放宽，但必须有上限，否则断网时会永远转圈。
constexpr int kDownloadTimeoutMs = 30000;

QString userAgent()
{
    return QStringLiteral("cxk-tools/%1").arg(QCoreApplication::applicationVersion());
}

/// 下载落盘目录：临时目录下的固定子目录，重装/退出后由系统清理。
QString downloadDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
           + QStringLiteral("/cxk-tools-update");
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
}

UpdateChecker::~UpdateChecker()
{
    // 析构时还挂着请求说明正在退出：abort 掉，别让 finished 回调碰已销毁的成员
    if (m_feedReply)
        m_feedReply->abort();
    if (m_downloadReply)
        m_downloadReply->abort();
}

QUrl UpdateChecker::feedUrl()
{
    // 覆盖口只用于本地验证（指向 file:// 的假清单）与将来可能的内网镜像
    const QByteArray override = qgetenv("XTOOLS_UPDATE_FEED");
    if (!override.isEmpty())
        return QUrl::fromEncoded(override);
    return QUrl(QString::fromLatin1(kFeedUrl));
}

void UpdateChecker::scheduleStartupCheck()
{
    Settings &settings = Settings::instance();

    if (!settings.autoCheckUpdate()) {
        qInfo() << "UpdateChecker: 自动检查已关闭，跳过启动检查";
        return;
    }

    const QDateTime last = settings.lastUpdateCheck();
    if (last.isValid()) {
        const qint64 elapsed = last.secsTo(QDateTime::currentDateTimeUtc());
        if (elapsed >= 0 && elapsed < qint64(kCheckIntervalHours) * 3600) {
            qInfo() << "UpdateChecker: 距上次检查不足 24 小时，跳过";
            return;
        }
    }

    QTimer::singleShot(kStartupDelayMs, this, [this] {
        m_silent = true;
        qInfo() << "UpdateChecker: 启动静默检查" << feedUrl().toString();
        requestFeed();
    });
}

void UpdateChecker::check()
{
    m_silent = false;

    // 已有请求在飞就先掐掉：用户连点两下「检查更新」不该压两个请求
    if (m_feedReply) {
        m_feedReply->abort();
        m_feedReply = nullptr;
    }
    requestFeed();
}

void UpdateChecker::requestFeed()
{
    emit checking();

    QNetworkRequest request(feedUrl());
    request.setTransferTimeout(kFeedTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    // CDN 会缓存 latest.json；发新版后立刻检查却拿到旧清单是最难查的一类问题
    request.setRawHeader("Cache-Control", "no-cache");

    m_feedReply = m_nam.get(request);
    connect(m_feedReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_feedReply;
        m_feedReply = nullptr;
        if (!reply)
            return;
        handleFeedFinished(reply);
        reply->deleteLater();
    });
}

void UpdateChecker::handleFeedFinished(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        // 404 多半是「仓库还没发过版」，说人话比抛错误码有用
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (status.isValid() && status.toInt() == 404)
            emit failed(QStringLiteral("更新源还没有发布过版本（HTTP 404）"));
        else
            emit failed(QStringLiteral("检查更新失败：%1").arg(reply->errorString()));
        return;
    }

    update::Info info;
    QString parseError;
    if (!update::parseLatestJson(reply->readAll(), &info, &parseError)) {
        emit failed(QStringLiteral("更新清单无法解析：%1").arg(parseError));
        return;
    }

    // 只有真正拿到并解析成功才算「检查过」，否则网络抖动会让节流把用户锁 24 小时
    Settings::instance().setLastUpdateCheck(QDateTime::currentDateTimeUtc());

    const QString current = QCoreApplication::applicationVersion();
    qInfo() << "UpdateChecker: 远端版本" << info.version << "本机版本" << current;

    if (update::compareVersions(info.version, current) > 0) {
        // 静默检查时尊重用户点过的「稍后」：同一个版本不再反复骚扰；
        // 手动检查仍然提示，否则用户会觉得按钮坏了。
        if (m_silent && Settings::instance().skippedVersion() == info.version) {
            qInfo() << "UpdateChecker: 用户已跳过" << info.version << "，静默检查不再提示";
            return;
        }
        emit updateAvailable(info);
        return;
    }

    emit upToDate(current);
}

void UpdateChecker::download(const update::Asset &asset)
{
    if (m_downloadReply) {
        emit downloadFailed(QStringLiteral("上一个下载还没结束"));
        return;
    }
    if (asset.url.isEmpty()) {
        emit downloadFailed(QStringLiteral("更新清单里的下载地址为空"));
        return;
    }

    const QString dir = downloadDir();
    if (!QDir().mkpath(dir)) {
        emit downloadFailed(QStringLiteral("无法创建下载目录：%1").arg(QDir::toNativeSeparators(dir)));
        return;
    }

    // 只取文件名，避免清单里带上 "../" 之类的东西把文件写到别处
    const QString path = dir + QLatin1Char('/') + QFileInfo(asset.name).fileName();
    m_downloadFile.setFileName(path);
    if (!m_downloadFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadFailed(QStringLiteral("无法写入临时文件：%1")
                                .arg(QDir::toNativeSeparators(path)));
        return;
    }

    m_downloadAsset = asset;
    m_received = 0;
    m_total = asset.size;
    m_hash.reset();

    QNetworkRequest request(asset.url);
    request.setTransferTimeout(kDownloadTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());

    m_downloadReply = m_nam.get(request);
    qInfo() << "UpdateChecker: 开始下载" << asset.name << "from" << asset.url.toString();

    connect(m_downloadReply, &QNetworkReply::readyRead, this, [this] {
        if (!m_downloadReply)
            return;
        const QByteArray chunk = m_downloadReply->readAll();
        if (chunk.isEmpty())
            return;

        m_hash.addData(chunk);
        if (m_downloadFile.write(chunk) != chunk.size()) {
            m_downloadReply->abort();
            return;
        }
        m_received += chunk.size();

        if (m_total <= 0)
            m_total = m_downloadReply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        emit downloadProgress(m_received, m_total);
    });

    connect(m_downloadReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_downloadReply;
        m_downloadReply = nullptr;
        if (!reply)
            return;

        // 收尾可能还有没读的尾巴（readyRead 不保证在 finished 前再触发一次）
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            m_hash.addData(tail);
            m_downloadFile.write(tail);
            m_received += tail.size();
        }

        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString message = ok ? QString() : reply->errorString();
        reply->deleteLater();
        finishDownload(ok, message);
    });
}

void UpdateChecker::finishDownload(bool ok, const QString &message)
{
    const QString path = m_downloadFile.fileName();
    m_downloadFile.close();

    if (!ok) {
        QFile::remove(path);
        emit downloadFailed(QStringLiteral("下载失败：%1").arg(message));
        return;
    }

    // 清单给了校验值就必须对上：下到半截、被运营商插广告、代理返回错误页，
    // 单靠"文件能打开"是发现不了的。
    if (!m_downloadAsset.sha256.isEmpty()) {
        const QString actual = QString::fromLatin1(m_hash.result().toHex());
        if (actual.compare(m_downloadAsset.sha256, Qt::CaseInsensitive) != 0) {
            QFile::remove(path);
            emit downloadFailed(QStringLiteral("安装包校验失败（SHA256 不匹配），已删除"));
            return;
        }
    }

    if (m_downloadAsset.size > 0 && m_received != m_downloadAsset.size) {
        QFile::remove(path);
        emit downloadFailed(QStringLiteral("安装包不完整（%1/%2 字节），已删除")
                                .arg(m_received)
                                .arg(m_downloadAsset.size));
        return;
    }

    qInfo() << "UpdateChecker: 下载完成" << path << m_received << "bytes";
    emit downloadReady(path, m_downloadAsset);
}

bool UpdateChecker::isInstalledBuild()
{
#if defined(Q_OS_WIN)
    // Inno Setup 会在安装目录放 unins000.exe；有它就是「安装版」，否则是便携版。
    // 用文件存在性判断而不是查注册表：便携版拷来即用，注册表里根本没有记录。
    const QString dir = QCoreApplication::applicationDirPath();
    return QFileInfo::exists(dir + QStringLiteral("/unins000.exe"));
#else
    return false;
#endif
}

bool UpdateChecker::launchInstaller(const QString &filePath, QString *error)
{
#if defined(Q_OS_WIN)
    // 静默参数与 installer/windows/cxk-tools.iss 的 [Run]/CloseApplications 配套：
    //   /SILENT            只显示进度，不弹页面
    //   /CLOSEAPPLICATIONS Restart Manager 关掉还在跑的旧实例（我们自己先退也行）
    //   /NORESTART         不要重启系统
    // 装完由 .iss 的 [Run] 段把新版本拉起来，所以这里不需要再启动一次。
    QStringList args{QStringLiteral("/SILENT"),
                     QStringLiteral("/CLOSEAPPLICATIONS"),
                     QStringLiteral("/NORESTART")};

    // 装到 %LOCALAPPDATA% 的副本是「仅为当前用户安装」（向导里选了 non-admin）。
    // 这时必须补 /CURRENTUSER，否则 Inno 按 PrivilegesRequired=admin 去提权：
    // 静默流程里 UAC 被拒绝一次，升级就悄无声息地什么都没做，用户只看到
    // 「安装包已下载、版本没变」。装在 Program Files 时保持默认（本来就需要提权）。
    const QString exeDir = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath());
    const QString localAppData = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    if (update::isPerUserInstallDir(exeDir, localAppData))
        args << QStringLiteral("/CURRENTUSER");

    if (!QProcess::startDetached(filePath, args)) {
        if (error)
            *error = QStringLiteral("无法启动安装程序：%1").arg(QDir::toNativeSeparators(filePath));
        return false;
    }
    return true;
#else
    Q_UNUSED(filePath)
    if (error)
        *error = QStringLiteral("当前平台不支持静默安装，请手动打开安装包");
    return false;
#endif
}

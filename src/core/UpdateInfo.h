#pragma once

// ---------------------------------------------------------------------------
// 更新清单（latest.json）的解析与版本比较 —— 纯逻辑头文件。
//
// 为什么单独抽出来：更新功能里最容易出错的是「版本号比大小」和「清单字段缺失」，
// 这两件事都不需要网络也不需要窗口，写成纯函数就能用一个小程序离线断言，
// 而不是等到发版时拿真实 release 试。
//
// 清单由 CI 生成（scripts/gen-release-manifest.py），字段契约见 AGENTS.md：
//   { "version":"0.2.0", "tag":"v0.2.0", "page_url":"…", "notes":"…",
//     "assets":[ {"platform":"windows-x64","kind":"installer",
//                 "name":"cxk-tools-0.2.0-windows-x64-setup.exe",
//                 "url":"https://…","sha256":"…","size":123} ] }
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QSysInfo>
#include <QUrl>
#include <QVector>

namespace update {

/// 清单里的一个下载项。
struct Asset
{
    QString platform;   ///< windows-x64 / macos-arm64 / macos-x64 / linux-x64
    QString kind;       ///< installer / portable / dmg / tarball
    QString name;       ///< 文件名，用于落盘
    QUrl url;
    QString sha256;     ///< 小写十六进制；为空表示清单没给校验值
    qint64 size = 0;
};

/// 一份清单。
struct Info
{
    QString version;    ///< 不带 v 前缀，如 0.2.0
    QString tag;        ///< 如 v0.2.0
    QString notes;      ///< release notes（markdown）
    QUrl pageUrl;       ///< 打不开下载项时给用户看的发布页
    QVector<Asset> assets;

    bool isValid() const { return !version.isEmpty(); }
};

/// 当前平台在清单里的 key。macOS 还要按 CPU 区分 arm64 与 x64。
///
/// 未知平台返回空字符串：调用方据此提示「当前平台没有自动更新包」，
/// 而不是随便挑一个包下载下来装不上。
inline QString platformKey()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows-x64");
#elif defined(Q_OS_MACOS)
    return QSysInfo::currentCpuArchitecture().startsWith(QLatin1String("arm"))
               ? QStringLiteral("macos-arm64")
               : QStringLiteral("macos-x64");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux-x64");
#else
    return QString();
#endif
}

/// 去掉可选的 v/V 前缀再按 . 切段。多余的空白也一并去掉，清单是人工可改的。
inline QStringList versionParts(const QString &version)
{
    QString v = version.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V')))
        v = v.mid(1);
    return v.split(QLatin1Char('.'), Qt::SkipEmptyParts);
}

/// 取一段的数值部分："3-beta" 取 3，"0" 取 0，非数字开头取 0。
/// 预发布后缀（-beta/-rc）不参与比较：本项目只发正式版，多一层语义反而容易出错。
inline int segmentValue(const QString &segment)
{
    int value = 0;
    for (const QChar ch : segment) {
        if (!ch.isDigit())
            break;
        value = value * 10 + ch.digitValue();
    }
    return value;
}

/// 语义化版本比较：a<b 返回 -1，相等 0，a>b 返回 1。
/// 段数不同按 0 补齐，因此 1.0 与 1.0.0 相等。
inline int compareVersions(const QString &lhs, const QString &rhs)
{
    const QStringList a = versionParts(lhs);
    const QStringList b = versionParts(rhs);
    const int count = qMax(a.size(), b.size());
    for (int i = 0; i < count; ++i) {
        const int va = i < a.size() ? segmentValue(a.at(i)) : 0;
        const int vb = i < b.size() ? segmentValue(b.at(i)) : 0;
        if (va != vb)
            return va < vb ? -1 : 1;
    }
    return 0;
}

/// 解析清单。失败时把可读原因写进 error（给设置页/日志用），并保证 out 被清空。
inline bool parseLatestJson(const QByteArray &json, Info *out, QString *error)
{
    if (out)
        *out = Info{};

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("更新清单不是合法的 JSON：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();

    Info info;
    info.version = root.value(QStringLiteral("version")).toString().trimmed();
    info.tag = root.value(QStringLiteral("tag")).toString().trimmed();
    info.notes = root.value(QStringLiteral("notes")).toString();
    info.pageUrl = QUrl(root.value(QStringLiteral("page_url")).toString());

    if (info.version.isEmpty()) {
        if (error)
            *error = QStringLiteral("更新清单缺少 version 字段");
        return false;
    }

    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject object = value.toObject();
        Asset asset;
        asset.platform = object.value(QStringLiteral("platform")).toString();
        asset.kind = object.value(QStringLiteral("kind")).toString();
        asset.name = object.value(QStringLiteral("name")).toString();
        asset.url = QUrl(object.value(QStringLiteral("url")).toString());
        asset.sha256 = object.value(QStringLiteral("sha256")).toString().toLower();
        asset.size = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());

        // 缺 platform 或 url 的条目一律丢弃：宁可少一个包，也不要下载一个空地址
        if (asset.platform.isEmpty() || !asset.url.isValid() || asset.name.isEmpty())
            continue;
        info.assets.append(asset);
    }

    if (info.assets.isEmpty()) {
        if (error)
            *error = QStringLiteral("更新清单里没有可用的下载项");
        return false;
    }

    if (out)
        *out = info;
    return true;
}

/// 在当前平台挑一个下载项：优先 preferredKind（Windows 用 installer、macOS 用 dmg），
/// 否则退回该平台第一个可用项。找不到返回 nullptr。
inline const Asset *pickAsset(const Info &info, const QString &preferredKind)
{
    const QString key = platformKey();
    if (key.isEmpty())
        return nullptr;

    const Asset *fallback = nullptr;
    for (const Asset &asset : info.assets) {
        if (asset.platform != key)
            continue;
        if (asset.kind == preferredKind)
            return &asset;
        if (!fallback)
            fallback = &asset;
    }
    return fallback;
}

/// 判断某个可执行文件所在目录是不是「按当前用户安装」的位置（Windows 专用判断）。
///
/// 为什么需要：Inno 的 PrivilegesRequired=admin 会让静默升级去提权，而按用户安装的副本
/// 本来就不需要提权；一旦用户在 UAC 上点了「否」，/SILENT 的升级会**静默地什么都不做**，
/// 界面只能看到「安装包启动了、版本没变」。所以升级前要先认出安装范围，按用户装就补
/// /CURRENTUSER。
///
/// 参数都用正斜杠（调用方先做 QDir::fromNativeSeparators）。比较时必须带上分隔符，
/// 否则 `C:\Users\a\AppData\LocalBackup\x` 会被误判成在 `...\AppData\Local` 下。
inline bool isPerUserInstallDir(const QString &exeDir, const QString &localAppData)
{
    if (exeDir.isEmpty() || localAppData.isEmpty())
        return false;

    QString root = localAppData;
    if (!root.endsWith(QLatin1Char('/')))
        root += QLatin1Char('/');

    const QString dir = exeDir.endsWith(QLatin1Char('/')) ? exeDir : exeDir + QLatin1Char('/');
    return dir.startsWith(root, Qt::CaseInsensitive);
}

} // namespace update

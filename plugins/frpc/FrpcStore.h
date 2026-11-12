#pragma once

// ---------------------------------------------------------------------------
// frpc 配置档的本地存档。
//
// 存档内容：一组「配置档」（名字 + frpc 可执行文件路径 + 配置原文）+ 上次选中的下标。
// 为什么用一份 JSON 而不是 QSettings/INI：配置原文是多行文本，INI 里要转义换行，
// 读出来经常多一个 `\n` 或者少一段注释，用户会很困惑「我保存的注释去哪了」。
// 整份 JSON 加载一次、保存一次全量，代码短，也不会出现「删了档还在」。
//
// 文件位置用 QStandardPaths::AppConfigLocation，与 ipquery / httpclient 的存档同层：
//   Windows  %APPDATA%/.../cxk-tools/frpc.json
//   macOS    ~/Library/Preferences/.../frpc.json
//   Linux    ~/.config/.../frpc.json
// 注意只有**配置文本与可执行文件路径**存这里；运行状态（pid）与日志在 AppLocalData 下，
// 见 FrpcRunner.h —— 配置是需要备份的东西，pid/日志不该混进来。
// ---------------------------------------------------------------------------

#include <QString>
#include <QVector>

namespace frpc {

/// 一份配置档。
struct Profile
{
    QString id;       ///< 稳定标识（UUID），用来命名临时配置文件与日志文件；重命名也不会变
    QString name;     ///< 展示名
    QString binPath;  ///< frpc 可执行文件路径；空表示「用自动探测的结果」
    QString config;   ///< 配置原文（TOML / INI）
};

class Store
{
public:
    /// path 为空时用默认位置。测试可以传临时路径，避免碰到用户配置。
    explicit Store(QString path = QString());

    QString filePath() const { return m_path; }

    /// 读全部配置档。文件不存在 / 坏档 / 版本不对时返回空列表（由调用方塞默认档），
    /// 不抛异常也不弹窗：存档坏了最多是从头配一次，比工具打不开好。
    QVector<Profile> profiles() const;
    void setProfiles(const QVector<Profile> &profiles);

    /// 上次选中的档下标。读回来时可能已经越界（用户删过档），调用方负责钳制。
    int activeIndex() const;
    void setActiveIndex(int index);

    /// 保证至少有一份配置档：空存档时写入一份带模板的「默认配置」。
    /// 返回最终列表，调用方直接用，不必再读一次。
    QVector<Profile> ensureDefaults();

    /// 新档的 id。用 UUID 而不是「配置档名字」：名字可以重名、可以带斜杠与空格，
    /// 而 id 要拼进文件路径，必须对所有文件系统都安全。
    static QString newId();

private:
    QString m_path;
};

} // namespace frpc

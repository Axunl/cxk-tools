#pragma once

// ---------------------------------------------------------------------------
// 「上次脚本 + 最近运行」的落盘。
//
// 为什么不复用宿主的 Settings 单例：那是主程序内部类，插件只链 xtools_api，
// 根本看不到。这里按 plugins/finance 的做法自己开一个 ini（同样放在
// AppConfigLocation 下，与主配置同目录、互不干扰）。
//
// 为什么用 QSettings(path, IniFormat) 而不是默认构造：默认构造在 Windows 上写
// 注册表、在 Linux 上写 ~/.config，三平台行为不一致，排查问题时很难对账。
// setFallbacksEnabled(false) 也不能省：否则未写入的键会去系统注册表找同名值，
// 出现「明明没存过却有值」。
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>

namespace jsrun {

/// 保留多少条成功运行的片段。10 条足够覆盖「刚才那段」的回填需求，
/// 再多会让下拉框变成一个小型文件管理器。
inline constexpr int kMaxHistory = 10;

/// ini 的绝对路径（`<AppConfigLocation>/jsrun.ini`），目录不存在时会创建。
QString iniPath();

/// 极简键值存储。历史用 QSettings 的数组 API 存，避免自己拼分隔符——
/// 脚本里出现任何分隔符都是合法的。
class Store
{
public:
    /// 默认落到 iniPath()；显式传路径是为了让临时验证工程能用 QTemporaryDir。
    explicit Store(QString path = iniPath());

    QString path() const { return m_path; }

    /// 上次编辑/运行的脚本。空表示从没存过（首次进入要载入示例）。
    QString lastScript() const;
    void setLastScript(const QString &code);

    /// 最近成功运行的片段，最新的在前。
    QStringList history() const;

    /// 记录一次成功运行：去重后插到最前，超过 kMaxHistory 截尾。
    /// 只有成功运行的脚本才进历史——失败片段回填的价值很低，还会挤掉有用的。
    void addHistory(const QString &code);

private:
    QString m_path;
};

} // namespace jsrun

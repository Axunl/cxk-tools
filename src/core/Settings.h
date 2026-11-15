#pragma once

// ---------------------------------------------------------------------------
// 配置读写（单例）。
//
// 为什么单例：配置是全应用共享的低频状态，注入到每个类里只会让构造函数变脏。
// 生命周期贯穿进程，不持有 UI 指针，因此不存在销毁顺序问题。
//
// 落盘位置：QStandardPaths::AppConfigLocation 下的 cxk-tools.ini（明文 INI，
// 方便用户手工编辑/排查），Windows 大约在 %LOCALAPPDATA%\cxk\cxk-tools\。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QDateTime>
#include <QKeySequence>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>

class Settings : public QObject
{
    Q_OBJECT

public:
    /// 「最近使用」的保留上限，首页分组与写入裁剪共用这一个值。
    static constexpr int kRecentLimit = 8;

    static Settings &instance();

    QKeySequence hotkey() const;
    void setHotkey(const QKeySequence &seq);

    bool startWithSystem() const;
    void setStartWithSystem(bool on);

    /// 界面主题："system"（跟随系统）/ "light" / "dark"。默认跟随系统。
    /// 存字符串而不是枚举：ini 是给人看的，写 "dark" 比写 "2" 更能说明问题。
    /// 合法值的解析与回退在 Theme::modeFromString 里，本类只负责存取。
    QString themeMode() const;
    void setThemeMode(const QString &mode);

    /// 最近使用过的工具 id，按时间倒序（最新的在前）。
    /// limit <= 0 表示不裁剪——写入时的裁剪逻辑要拿全量才能知道该删谁。
    QStringList recentToolIds(int limit = kRecentLimit) const;

    /// 记一次「用户主动打开了这个工具」。空白 id 直接忽略。
    /// 与 usage 分开存：usage 是 int 计数，塞进时间戳会让老用户的频次数据作废。
    void touchRecent(const QString &toolId);

    /// 窗口几何（QWidget::saveGeometry 的字节流），键为 ui/<name>/geometry。
    /// name 里的 '/' 会被换成 '_'，避免带斜杠的插件 id 把 QSettings 的组层级打断。
    QByteArray windowGeometry(const QString &name) const;
    void setWindowGeometry(const QString &name, const QByteArray &geo);

    /// 使用频次，参与搜索排序加成（见 ToolRegistry）。
    int usage(const QString &toolId) const;
    void bumpUsage(const QString &toolId);

    /// 启动时自动检查更新（默认开）。关掉之后只有用户手点「检查更新」才联网。
    bool autoCheckUpdate() const;
    void setAutoCheckUpdate(bool on);

    /// 上次成功检查更新的时间（UTC）。用于「24 小时内不重复联网」的节流。
    QDateTime lastUpdateCheck() const;
    void setLastUpdateCheck(const QDateTime &when);

    /// 用户点过「稍后」的版本号：静默检查不再为它弹气泡（手动检查仍然提示）。
    QString skippedVersion() const;
    void setSkippedVersion(const QString &version);

    /// 网络代理模式："auto"（默认，依次试环境变量 / 系统代理 / 本机常见端口）/
    /// "direct"（一律直连）/ "manual"（用下面的 host+port+type）。
    /// 存字符串的理由同主题：ini 是给人看的，写 "auto" 比写 "0" 清楚。
    QString proxyMode() const;
    void setProxyMode(const QString &mode);

    /// 手动代理的协议（"http" / "socks5"）。auto 模式用不到，留着是为了
    /// 用户从「自动」切到「手动」时上次填的值还在。
    QString proxyType() const;
    void setProxyType(const QString &type);

    /// 手动代理的主机与端口。默认 127.0.0.1:7890（Clash 的混合端口），
    /// 也就是国内最常见的本机代理。
    QString proxyHost() const;
    void setProxyHost(const QString &host);
    int proxyPort() const;
    void setProxyPort(int port);

    QString configFilePath() const;
    void sync();

    /// 默认热键。刻意不用 Alt+Space：它确实能注册（实测 RegisterHotKey 成功、
    /// 也能收到 WM_HOTKEY），但代价是抢掉所有窗口的系统菜单，而且录入时
    /// Qt 根本收不到这个组合（见 HotkeyButton::nativeEvent）。用户想要就让他自己改，
    /// 默认值不给人添这种麻烦。
    static QKeySequence defaultHotkey();

private:
    Settings();

    /// 把自启状态落到系统：Windows 写注册表 Run 项，Linux 写 XDG .desktop，
    /// 其他平台（含 macOS）暂不支持，只记录警告，不假装成功。
    void applyAutostart(bool on);
    static QString autostartCommand();

    QSettings m_settings;
};

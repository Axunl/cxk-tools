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

#include <QKeySequence>
#include <QObject>
#include <QSettings>
#include <QString>

class Settings : public QObject
{
    Q_OBJECT

public:
    static Settings &instance();

    QKeySequence hotkey() const;
    void setHotkey(const QKeySequence &seq);

    bool startWithSystem() const;
    void setStartWithSystem(bool on);

    /// 上次使用的工具 id，启动时用来恢复现场（不会因此累计使用频次）。
    QString lastToolId() const;
    void setLastToolId(const QString &id);

    /// 使用频次，参与搜索排序加成（见 ToolRegistry）。
    int usage(const QString &toolId) const;
    void bumpUsage(const QString &toolId);

    QString configFilePath() const;
    void sync();

    /// 默认热键。刻意不用 Alt+Space：Windows 把它保留给窗口系统菜单，
    /// RegisterHotKey 必定失败。
    static QKeySequence defaultHotkey();

private:
    Settings();

    /// 把自启状态落到系统：Windows 写注册表 Run 项，Linux 写 XDG .desktop，
    /// 其他平台（含 macOS）暂不支持，只记录警告，不假装成功。
    void applyAutostart(bool on);
    static QString autostartCommand();

    QSettings m_settings;
};

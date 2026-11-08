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

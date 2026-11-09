#pragma once

// ---------------------------------------------------------------------------
// 设置页：常规（热键/自启/主题）、工具与插件、更新、关于。
//
// 热键用自定义按钮 HotkeyButton 现场捕获组合键，而不是下拉框拼装 ——
// 后者在跨平台时很难表达 macOS 的 ⌘/⌥，也让用户点几下才能设完。
// ---------------------------------------------------------------------------

#include <QKeySequence>
#include <QPushButton>
#include <QString>
#include <QWidget>

class Settings;
class PluginManager;
class QCheckBox;
class QComboBox;
class QLabel;

/// 点击后进入"捕获"状态，按下的第一个带修饰键的组合即生效。
class HotkeyButton : public QPushButton
{
    Q_OBJECT

public:
    explicit HotkeyButton(QWidget *parent = nullptr);

    void setSequence(const QKeySequence &seq);
    QKeySequence sequence() const { return m_sequence; }

signals:
    void sequenceCaptured(const QKeySequence &seq);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
#ifdef Q_OS_WIN
    /// 兜住 Qt 在 Windows 上不派发的那一小撮组合（目前是 Alt+Space：
    /// 系统把它当窗口菜单，连 QKeyEvent 都不生成）。见 .cpp 里的说明。
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
    void startCapture();
    void stopCapture();

    /// 捕获成功后统一走这里：写入 m_sequence、退出捕获状态、发信号。
    /// keyPressEvent 与 nativeEvent 两条入口共用，避免两处逻辑漂移。
    void acceptSequence(Qt::KeyboardModifiers mods, int key);

    QKeySequence m_sequence;
    bool m_capturing = false;
};

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    SettingsPage(Settings *settings, PluginManager *plugins, QWidget *parent = nullptr);

    /// 热键注册结果回显（绿色已生效 / 红色失败原因）。
    void setHotkeyStatus(const QString &text, bool ok);

    /// 更新检查的状态回显（「更新」分组里那行小字）。
    void setUpdateStatus(const QString &text, bool ok);

    /// 检查进行中：按钮置灰，避免连点压出多个请求。
    void setUpdateChecking(bool checking);

signals:
    void hotkeyChanged(const QKeySequence &seq);
    void restartRequested();
    /// 用户点了「检查更新」；真正的联网在 AppController 里做。
    void checkUpdateRequested();

private:
    QWidget *buildGeneralGroup();
    QWidget *buildPluginGroup();
    QWidget *buildUpdateGroup();
    QWidget *buildAboutGroup();

    Settings *m_settings = nullptr;
    PluginManager *m_plugins = nullptr;
    HotkeyButton *m_hotkeyButton = nullptr;
    QLabel *m_hotkeyStatus = nullptr;
    QComboBox *m_themeCombo = nullptr;
    QPushButton *m_updateButton = nullptr;
    QLabel *m_updateStatus = nullptr;
    QCheckBox *m_autoCheck = nullptr;
};

#pragma once

// ---------------------------------------------------------------------------
// 设置页：常规（热键/自启）、工具与插件、关于。
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

private:
    void startCapture();
    void stopCapture();

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

signals:
    void hotkeyChanged(const QKeySequence &seq);
    void restartRequested();

private:
    QWidget *buildGeneralGroup();
    QWidget *buildPluginGroup();
    QWidget *buildAboutGroup();

    Settings *m_settings = nullptr;
    PluginManager *m_plugins = nullptr;
    HotkeyButton *m_hotkeyButton = nullptr;
    QLabel *m_hotkeyStatus = nullptr;
};

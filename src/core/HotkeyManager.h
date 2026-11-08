#pragma once

// ---------------------------------------------------------------------------
// 全局热键。
//
// 平台实现：
//   * Windows —— RegisterHotKey 注册到 message-only 窗口（HWND_MESSAGE），
//                在 WndProc 里收 WM_HOTKEY；
//   * Linux   —— X11 XGrabKey 抓根窗口，按键事件从 Qt 的 native event filter
//                里取 xcb 事件；
//   * macOS   —— 尚未实现（需要 Carbon RegisterEventHotKey 或辅助功能 API），
//                registerSequence() 会返回明确的错误文案，绝不静默失败。
//
// 失败语义：registerSequence() 用返回值 + errorMessage 报告原因，
// 上层（AppController）负责把原因显示到界面/托盘，而不是只在日志里留一行。
// ---------------------------------------------------------------------------

#include <QKeySequence>
#include <QObject>
#include <QString>

#ifdef Q_OS_LINUX
#include <QAbstractNativeEventFilter>
#endif

class HotkeyManager : public QObject
#ifdef Q_OS_LINUX
    , public QAbstractNativeEventFilter
#endif
{
    Q_OBJECT

public:
    explicit HotkeyManager(QObject *parent = nullptr);
    ~HotkeyManager() override;

    /// 注册（或换绑）全局热键。会先注销旧热键，因此可以反复调用。
    /// @param errorMessage 失败时写入用户可读的中文原因（可能为 nullptr）
    /// @return 注册成功与否
    bool registerSequence(const QKeySequence &seq, QString *errorMessage);

    void unregister();
    bool isRegistered() const { return m_registered; }

signals:
    /// 热键被按下。注意信号在 GUI 线程发出（底层回调里只做 QueuedConnection 转投）。
    void activated();

public slots:
    /// 供底层回调通过 QMetaObject::invokeMethod("onHotkey") 投递到 Qt 事件循环。
    void onHotkey();

#ifdef Q_OS_LINUX
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
    /// Windows 专用的消息接收窗口（HWND_MESSAGE，不可见）；其他平台空实现。
    bool ensureSinkWindow();
    void destroySinkWindow();

    void *m_sink = nullptr;          ///< Windows: HWND；其他平台不使用
    bool m_registered = false;
    QKeySequence m_sequence;

    unsigned int m_keycode = 0;      ///< Linux: X11 KeyCode
    unsigned int m_mods = 0;         ///< Linux: X11 修饰位（不含 CapsLock/NumLock）
};

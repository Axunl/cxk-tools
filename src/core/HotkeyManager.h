#pragma once

// ---------------------------------------------------------------------------
// 全局热键。
//
// 平台实现：
//   * Windows —— RegisterHotKey 注册到 message-only 窗口（HWND_MESSAGE），
//                在 WndProc 里收 WM_HOTKEY；另外挂一个 WH_KEYBOARD_LL 兜底，
//                防止按键在进入系统热键匹配前被别的进程的键盘钩子吃掉；
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

    /// Windows 兜底通道：低级键盘钩子。
    ///
    /// RegisterHotKey 注册成功后，按键仍可能在系统热键匹配之前被别的进程（远程控制 /
    /// 键盘增强 / 输入法）的 WH_KEYBOARD_LL 钩子截走，表现就是「注册成功、按了没反应」。
    /// 这个钩子只负责「看到键」并补一次唤起，见 .cpp 里的详细说明。
    ///
    /// @param virtualKey  Win32 虚拟键码（VK_*）
    /// @param winModifiers RegisterHotKey 的 MOD_* 位组合
    ///
    /// 非 Windows 平台是空实现（X11 的 XGrabKey 没有这个问题，macOS 还没实现热键）。
    void installKeyboardHook(unsigned int virtualKey, unsigned int winModifiers);
    void uninstallKeyboardHook();

    void *m_sink = nullptr;          ///< Windows: HWND；其他平台不使用
    bool m_registered = false;
    QKeySequence m_sequence;

    unsigned int m_keycode = 0;      ///< Linux: X11 KeyCode
    unsigned int m_mods = 0;         ///< Linux: X11 修饰位（不含 CapsLock/NumLock）
};

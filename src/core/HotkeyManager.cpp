#include "HotkeyManager.h"

#include <QDebug>

#if defined(Q_OS_WIN)

#include <windows.h>

#include <QDateTime>
#include <QPointer>
#include <QTimer>

namespace {

/// RegisterHotKey 的 id，进程内唯一即可。
constexpr int kHotkeyId = 0xB001;

/// message-only 窗口的窗口类名，重复创建时靠它复用已注册的类。
const wchar_t *kSinkClassName = L"CXKToolsHotkeySink";

// ---------------------------------------------------------------------------
// 兜底通道的状态（低级键盘钩子）
//
// 为什么需要它：RegisterHotKey 成功只代表「系统答应了」，不代表按键一定会变成
// WM_HOTKEY。按键要经过 WH_KEYBOARD_LL 钩子链才进入热键匹配，链上任何一环
// （远程控制、键盘增强、部分输入法、外设驱动）把这一次按键吃掉，我们就永远等
// 不到 WM_HOTKEY——日志里还是漂漂亮亮的「已注册」，用户按下去毫无反应。
//
// 实测（本机装了 ToDesk / Snipaste / Ditto 这类会挂键盘钩子的常驻程序）：
// 用 SendInput 合成的 Alt+Space 能正常派发 WM_HOTKEY，物理按键却常常没反应，
// 正是「钩子只截真实输入、放过注入事件」的典型表现。
//
// 所以再加一层：自己挂一个 WH_KEYBOARD_LL，看到目标组合键就补一次唤起。
// 为了不重复唤起，钩子命中后延迟 kHookFallbackDelayMs 再判断——如果这段
// 时间里 WM_HOTKEY 已经到了，说明系统热键本来就好使，这次就不管。
//
// 钩子**永远放行**按键（CallNextHookEx）：吞掉按键会让所有程序都收不到，
// 包括输入法，代价远大于收益。
// ---------------------------------------------------------------------------

/// 钩子命中后等系统热键的宽限期。取 150ms：正常情况 WM_HOTKEY 在同一轮事件循环
/// 里就到（实测 < 5ms），这个值只是给卡顿留余量，同时又短到用户察觉不到。
constexpr int kHookFallbackDelayMs = 150;

/// 同一个按键的自动重复抑制窗口。按住不放时钩子会收到连发的 KEYDOWN，
/// 但一个「按下」只该唤起一次（RegisterHotKey 侧靠 MOD_NOREPEAT 达到同样效果）。
constexpr qint64 kHookRepeatGuardMs = 400;

HHOOK g_keyboardHook = nullptr;
QPointer<HotkeyManager> g_hookOwner;   ///< 回调要转投到哪个对象（钩子回调在 GUI 线程）
unsigned int g_hookVirtualKey = 0;     ///< 要匹配的虚拟键码，0 表示不匹配任何键
unsigned int g_hookModifiers = 0;      ///< 要匹配的 MOD_* 位组合
qint64 g_lastWmHotkeyMs = 0;           ///< 最近一次收到 WM_HOTKEY 的时刻
qint64 g_lastHookFireMs = 0;           ///< 最近一次钩子命中的时刻（抑制自动重复）

/// 判断当前是否正好按着 expected 这组修饰键。
///
/// 读实时键状态（GetAsyncKeyState）而不是钩子事件里的标志位：KBDLLHOOKSTRUCT
/// 只带被按下那一个键的信息，修饰键得自己查。Alt+Space 走到这里时 Alt 是按住的，
/// 所以 VK_MENU 的高位必然是 1。
bool modifiersMatch(unsigned int expected)
{
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool win = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;

    unsigned int actual = 0;
    if (ctrl)  actual |= MOD_CONTROL;
    if (alt)   actual |= MOD_ALT;
    if (shift) actual |= MOD_SHIFT;
    if (win)   actual |= MOD_WIN;
    return actual == expected;
}

/// 低级键盘钩子回调。
///
/// 运行在安装钩子的线程（也就是 GUI 线程）上，因此可以安全地碰 Qt 对象；
/// 但它挡在系统输入通路上，**这里绝不能做重活**，否则整机输入都会卡。
/// 所以只做「比对 + 记时间戳 + 起一个定时器」，唤起逻辑全在事件循环里做。
///
/// 不区分 LLKHF_INJECTED：远程桌面/自动化注入的按键同样应该能唤起窗口，
/// 而且注入事件走的也是同一条输入路径，没有理由区别对待。
LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        && g_hookVirtualKey != 0 && g_hookOwner) {
        auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        if (static_cast<unsigned int>(info->vkCode) == g_hookVirtualKey
            && modifiersMatch(g_hookModifiers)) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - g_lastHookFireMs > kHookRepeatGuardMs) {
                g_lastHookFireMs = now;
                const QPointer<HotkeyManager> owner = g_hookOwner;
                QTimer::singleShot(kHookFallbackDelayMs, owner.data(), [owner, now] {
                    if (!owner)
                        return;
                    // 这段时间里系统热键已经派发过，说明 RegisterHotKey 好使，别重复唤起
                    if (g_lastWmHotkeyMs >= now)
                        return;
                    qInfo() << "HotkeyManager: WM_HOTKEY 未到达，由键盘钩子兜底唤起";
                    QMetaObject::invokeMethod(owner.data(), "onHotkey", Qt::QueuedConnection);
                });
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

/// Qt::Key -> Win32 Virtual-Key。
///
/// 只覆盖常用键：字母/数字/功能键按区间连续可以算出来，符号键必须逐个映射
/// （VK_OEM_* 的编号与键盘布局相关，没有规律）。
UINT toVirtualKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<UINT>('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<UINT>('0' + (key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));

    switch (key) {
    case Qt::Key_Space:        return VK_SPACE;
    case Qt::Key_Tab:          return VK_TAB;
    case Qt::Key_Return:
    case Qt::Key_Enter:        return VK_RETURN;
    case Qt::Key_Escape:       return VK_ESCAPE;
    case Qt::Key_Backspace:    return VK_BACK;
    case Qt::Key_Insert:       return VK_INSERT;
    case Qt::Key_Delete:       return VK_DELETE;
    case Qt::Key_Home:         return VK_HOME;
    case Qt::Key_End:          return VK_END;
    case Qt::Key_PageUp:       return VK_PRIOR;
    case Qt::Key_PageDown:     return VK_NEXT;
    case Qt::Key_Left:         return VK_LEFT;
    case Qt::Key_Right:        return VK_RIGHT;
    case Qt::Key_Up:           return VK_UP;
    case Qt::Key_Down:         return VK_DOWN;
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde:   return VK_OEM_3;
    case Qt::Key_Minus:        return VK_OEM_MINUS;
    case Qt::Key_Equal:        return VK_OEM_PLUS;
    case Qt::Key_BracketLeft:  return VK_OEM_4;
    case Qt::Key_BracketRight: return VK_OEM_6;
    case Qt::Key_Backslash:    return VK_OEM_5;
    case Qt::Key_Semicolon:    return VK_OEM_1;
    case Qt::Key_Apostrophe:   return VK_OEM_7;
    case Qt::Key_Comma:        return VK_OEM_COMMA;
    case Qt::Key_Period:       return VK_OEM_PERIOD;
    case Qt::Key_Slash:        return VK_OEM_2;
    default:                   return 0;
    }
}

/// message-only 窗口的消息处理。
///
/// 只处理 WM_HOTKEY；用户数据里存了 HotkeyManager*，用它把事件转投到 Qt 事件循环
/// （在 WndProc 里直接操作 UI 会死在错误的线程上下文/重入上）。
LRESULT CALLBACK sinkProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    Q_UNUSED(lParam)

    if (msg == WM_HOTKEY && wParam == kHotkeyId) {
        // 记时刻供钩子兜底判断「系统热键其实好使」，避免一次按键唤起两次
        g_lastWmHotkeyMs = QDateTime::currentMSecsSinceEpoch();
        auto *self = reinterpret_cast<HotkeyManager *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self)
            QMetaObject::invokeMethod(self, "onHotkey", Qt::QueuedConnection);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

#elif defined(Q_OS_LINUX)

#include <QCoreApplication>
#include <QGuiApplication>
#include <QtGui/qguiapplication_platform.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>

namespace {

/// X11 的「锁键」：CapsLock / NumLock 按下时会改变事件里的 state 位。
/// 要让热键在锁键开启时也生效，必须把 4 种组合全部 grab 一遍。
constexpr unsigned int kLockMasks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

/// 取 X11 Display。Wayland 会话下拿不到（返回 nullptr），调用方要给出明确提示。
Display *x11Display()
{
    // nativeInterface<T>() 是 QGuiApplication 的**成员**函数（不是静态函数），
    // 写成 QGuiApplication::nativeInterface<...>() 看起来像静态调用，GCC 直接报
    // "cannot call member function without object"。用 qGuiApp 宏（等价于把
    // QCoreApplication::instance() 转成 QGuiApplication*）。
    //
    // 也别换成 QGuiApplication::instance()：它的静态类型是 QCoreApplication*，
    // 模板成员在基类上不存在，照样编译不过。
    auto *app = qGuiApp;
    if (!app)
        return nullptr;   // 理论上建窗口后才会调到这里，纯防御
    auto *x11 = app->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->display() : nullptr;
}

/// Qt::Key -> X11 KeySym。与 Windows 那份表保持同样的覆盖范围。
KeySym toKeySym(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return XK_a + (key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return XK_0 + (key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return XK_F1 + (key - Qt::Key_F1);

    switch (key) {
    case Qt::Key_Space:        return XK_space;
    case Qt::Key_Tab:          return XK_Tab;
    case Qt::Key_Return:
    case Qt::Key_Enter:        return XK_Return;
    case Qt::Key_Escape:       return XK_Escape;
    case Qt::Key_Backspace:    return XK_BackSpace;
    case Qt::Key_Insert:       return XK_Insert;
    case Qt::Key_Delete:       return XK_Delete;
    case Qt::Key_Home:         return XK_Home;
    case Qt::Key_End:          return XK_End;
    case Qt::Key_PageUp:       return XK_Prior;
    case Qt::Key_PageDown:     return XK_Next;
    case Qt::Key_Left:         return XK_Left;
    case Qt::Key_Right:        return XK_Right;
    case Qt::Key_Up:           return XK_Up;
    case Qt::Key_Down:         return XK_Down;
    case Qt::Key_QuoteLeft:
    case Qt::Key_AsciiTilde:   return XK_grave;
    case Qt::Key_Minus:        return XK_minus;
    case Qt::Key_Equal:        return XK_equal;
    case Qt::Key_BracketLeft:  return XK_bracketleft;
    case Qt::Key_BracketRight: return XK_bracketright;
    case Qt::Key_Backslash:    return XK_backslash;
    case Qt::Key_Semicolon:    return XK_semicolon;
    case Qt::Key_Apostrophe:   return XK_apostrophe;
    case Qt::Key_Comma:        return XK_comma;
    case Qt::Key_Period:       return XK_period;
    case Qt::Key_Slash:        return XK_slash;
    default:                   return 0;
    }
}

/// XGrabKey 的失败是**异步**上报的（走 X 错误协议），没有返回值可看。
/// 这里用一个临时错误处理器把 BadAccess 记下来，配合 XSync 才能判断是否被占用。
bool g_grabFailed = false;

int grabErrorHandler(Display *, XErrorEvent *event)
{
    if (event && event->error_code == BadAccess)
        g_grabFailed = true;
    return 0;
}

} // namespace

#endif // platform

HotkeyManager::HotkeyManager(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_LINUX
    // Linux 的按键事件只能从 native event filter 拿到（Qt 不会把它派发成 QKeyEvent）
    if (auto *app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
#endif
}

HotkeyManager::~HotkeyManager()
{
    unregister();
    destroySinkWindow();
#ifdef Q_OS_LINUX
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(this);
#endif
}

bool HotkeyManager::ensureSinkWindow()
{
#ifdef Q_OS_WIN
    if (m_sink)
        return true;

    HINSTANCE inst = GetModuleHandleW(nullptr);

    // 类可能已经注册过（换绑热键时会重建窗口），先查再注册，避免重复注册失败
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(inst, kSinkClassName, &wc)) {
        WNDCLASSEXW fresh = {};
        fresh.cbSize = sizeof(fresh);
        fresh.lpfnWndProc = sinkProc;
        fresh.hInstance = inst;
        fresh.lpszClassName = kSinkClassName;
        if (!RegisterClassExW(&fresh)) {
            qWarning() << "HotkeyManager: RegisterClassEx 失败:" << GetLastError();
            return false;
        }
    }

    // HWND_MESSAGE：只收消息的窗口，不占任务栏、不可见
    HWND hwnd = CreateWindowExW(0, kSinkClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, inst, nullptr);
    if (!hwnd) {
        qWarning() << "HotkeyManager: CreateWindowEx 失败:" << GetLastError();
        return false;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_sink = hwnd;
    return true;
#else
    // Linux 直接抓根窗口，不需要额外窗口；macOS 走的是「不支持」分支
    return true;
#endif
}

void HotkeyManager::destroySinkWindow()
{
#ifdef Q_OS_WIN
    if (m_sink) {
        DestroyWindow(static_cast<HWND>(m_sink));
        m_sink = nullptr;
    }
#endif
}

void HotkeyManager::unregister()
{
#if defined(Q_OS_WIN)
    if (m_registered && m_sink) {
        UnregisterHotKey(static_cast<HWND>(m_sink), kHotkeyId);
        m_registered = false;
    }
#elif defined(Q_OS_LINUX)
    if (m_registered && m_keycode != 0) {
        if (Display *dpy = x11Display()) {
            const Window root = DefaultRootWindow(dpy);
            for (unsigned int lock : kLockMasks)
                XUngrabKey(dpy, static_cast<int>(m_keycode), m_mods | lock, root);
            XFlush(dpy);
        }
        m_keycode = 0;
        m_mods = 0;
        m_registered = false;
    }
#endif

    // 热键没了，兜底钩子也必须停匹配，否则老的组合键还会把窗口唤出来
    uninstallKeyboardHook();
}

void HotkeyManager::installKeyboardHook(unsigned int virtualKey, unsigned int winModifiers)
{
#ifdef Q_OS_WIN
    // 先更新匹配条件：换绑时钩子已经在了，只需换要比对的键
    g_hookVirtualKey = virtualKey;
    g_hookModifiers = winModifiers;
    g_hookOwner = this;

    if (g_keyboardHook)
        return;

    // 参数 3 用本模块句柄 + 线程 id 0：低级键盘钩子装在当前线程，回调也回到这里
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleW(nullptr), 0);
    if (!g_keyboardHook) {
        // 钩子只是兜底，装不上不影响 RegisterHotKey 这条主路，所以只记日志不算失败
        qWarning() << "HotkeyManager: 键盘钩子兜底安装失败:" << GetLastError();
        return;
    }
    qInfo() << "HotkeyManager: 键盘钩子兜底已就绪";
#else
    Q_UNUSED(virtualKey)
    Q_UNUSED(winModifiers)
#endif
}

void HotkeyManager::uninstallKeyboardHook()
{
#ifdef Q_OS_WIN
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    g_hookVirtualKey = 0;
    g_hookModifiers = 0;
    g_hookOwner = nullptr;
#endif
}

bool HotkeyManager::registerSequence(const QKeySequence &seq, QString *errorMessage)
{
    // 换绑场景：先彻底注销旧热键，避免「新键注册成功但旧键仍然生效」
    unregister();

    if (seq.isEmpty()) {
        if (errorMessage)
            *errorMessage = tr("热键为空");
        return false;
    }

#if defined(Q_OS_WIN)
    if (!ensureSinkWindow()) {
        if (errorMessage)
            *errorMessage = tr("无法创建热键接收窗口");
        return false;
    }

    // 只取第一个组合（不支持多段序列热键）
    const QKeyCombination combo = seq[0];

    UINT mods = 0;
    const Qt::KeyboardModifiers qmods = combo.keyboardModifiers();
    if (qmods & Qt::ControlModifier) mods |= MOD_CONTROL;
    if (qmods & Qt::AltModifier)     mods |= MOD_ALT;
    if (qmods & Qt::ShiftModifier)   mods |= MOD_SHIFT;
    if (qmods & Qt::MetaModifier)    mods |= MOD_WIN;

    const UINT vk = toVirtualKey(combo.key());
    if (vk == 0) {
        if (errorMessage)
            *errorMessage = tr("不支持的按键组合：%1").arg(seq.toString(QKeySequence::NativeText));
        return false;
    }

    // 裸键（无修饰键）注册成全局热键会抢走整个键盘输入，直接拒绝
    if (mods == 0) {
        if (errorMessage)
            *errorMessage = tr("全局热键至少需要一个修饰键（Ctrl/Alt/Shift/Win）");
        return false;
    }

    // MOD_NOREPEAT：按住不放只触发一次，否则会被连续唤起
    if (!RegisterHotKey(static_cast<HWND>(m_sink), kHotkeyId, mods | MOD_NOREPEAT, vk)) {
        const DWORD err = GetLastError();
        if (errorMessage) {
            *errorMessage = (err == ERROR_HOTKEY_ALREADY_REGISTERED)
                                ? tr("热键 %1 已被其他程序占用").arg(seq.toString(QKeySequence::NativeText))
                                : tr("注册热键失败（错误码 %1）").arg(err);
        }
        return false;
    }

    m_registered = true;
    m_sequence = seq;
    qInfo() << "HotkeyManager: 已注册" << seq.toString(QKeySequence::NativeText);

    // 注册成功不代表按键一定到得了：再挂一层键盘钩子兜底（只在注册成功时挂，
    // 组合键真被别人占走时仍如实报「已被占用」，不去抢别人的热键）
    installKeyboardHook(vk, mods);
    return true;

#elif defined(Q_OS_LINUX)
    Display *dpy = x11Display();
    if (!dpy) {
        // Wayland 下没有 X11 Display，这是环境限制，必须说清楚而不是假装支持
        if (errorMessage)
            *errorMessage = tr("当前不是 X11 会话（Wayland 下不支持全局热键）");
        return false;
    }

    const QKeyCombination combo = seq[0];

    unsigned int mods = 0;
    const Qt::KeyboardModifiers qmods = combo.keyboardModifiers();
    if (qmods & Qt::ControlModifier) mods |= ControlMask;
    if (qmods & Qt::AltModifier)     mods |= Mod1Mask;    // X11 里 Alt 通常是 Mod1
    if (qmods & Qt::ShiftModifier)   mods |= ShiftMask;
    if (qmods & Qt::MetaModifier)    mods |= Mod4Mask;    // Win/Super 通常是 Mod4

    const KeySym ks = toKeySym(combo.key());
    const KeyCode kc = ks ? XKeysymToKeycode(dpy, ks) : 0;
    if (kc == 0) {
        if (errorMessage)
            *errorMessage = tr("不支持的按键组合：%1").arg(seq.toString(QKeySequence::NativeText));
        return false;
    }
    if (mods == 0) {
        if (errorMessage)
            *errorMessage = tr("全局热键至少需要一个修饰键（Ctrl/Alt/Shift/Super）");
        return false;
    }

    const Window root = DefaultRootWindow(dpy);

    g_grabFailed = false;
    XErrorHandler previous = XSetErrorHandler(grabErrorHandler);
    for (unsigned int lock : kLockMasks)
        XGrabKey(dpy, static_cast<int>(kc), mods | lock, root, False, GrabModeAsync, GrabModeAsync);
    XSync(dpy, False);   // 把异步错误逼出来
    XSetErrorHandler(previous);

    if (g_grabFailed) {
        // 部分 grab 已生效，回滚掉，避免留下半注册状态
        for (unsigned int lock : kLockMasks)
            XUngrabKey(dpy, static_cast<int>(kc), mods | lock, root);
        XFlush(dpy);
        if (errorMessage)
            *errorMessage = tr("热键 %1 已被其他程序占用").arg(seq.toString(QKeySequence::NativeText));
        return false;
    }

    m_keycode = static_cast<unsigned int>(kc);
    m_mods = mods;
    m_registered = true;
    m_sequence = seq;
    qInfo() << "HotkeyManager: 已注册" << seq.toString(QKeySequence::NativeText);
    return true;

#else
    Q_UNUSED(seq)
    // macOS 需要 Carbon RegisterEventHotKey（或辅助功能授权 + 事件监听），尚未实现。
    // 明确告知用户，而不是留一个永不放出的热键。
    if (errorMessage)
        *errorMessage = tr("当前平台（macOS）暂未实现全局热键");
    return false;
#endif
}

#ifdef Q_OS_LINUX
bool HotkeyManager::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result)

    if (!m_registered || m_keycode == 0 || eventType != QByteArrayLiteral("xcb_generic_event_t"))
        return false;

    auto *event = static_cast<xcb_generic_event_t *>(message);
    // 高比特位可能带 sentinel 标记，比较前要掩掉
    if ((event->response_type & ~0x80) != XCB_KEY_PRESS)
        return false;

    auto *press = reinterpret_cast<xcb_key_press_event_t *>(event);
    if (press->detail != m_keycode)
        return false;

    // 比较前忽略 CapsLock / NumLock 造成的状态位差异
    const unsigned int state = press->state & ~(LockMask | Mod2Mask);
    if (state != m_mods)
        return false;

    QMetaObject::invokeMethod(this, "onHotkey", Qt::QueuedConnection);
    return true;   // 吞掉事件，不再继续派发
}
#endif

void HotkeyManager::onHotkey()
{
    emit activated();
}

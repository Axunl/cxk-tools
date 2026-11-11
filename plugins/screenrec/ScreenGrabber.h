#pragma once

// ---------------------------------------------------------------------------
// 抓屏（纯 header，只依赖 QtGui，可被临时工程直接 include）。
//
// 四个刻意的设计选择：
//
// 1) **整屏抓 + 内存裁剪**，不用 grabWindow(0, x, y, w, h) 抓区域。
//    Qt 文档说 window==0 时 x/y 以该屏幕左上角为原点，但 Windows 后端最终走
//    BitBlt(GetDC(NULL))，按的是虚拟桌面坐标；仓库里现有代码（取色器、OCR）
//    只在主屏（原点就是 0,0）上验证过，两种语义在主屏完全重合，无法据此推断副屏。
//    整屏抓完再裁，坐标链只有一个，出错概率低得多。
//
// 2) **Windows 上走原生 BitBlt 快路径**。实测（2560×1600）：QScreen::grabWindow
//    要 41ms，GetDC(NULL)+BitBlt 到 DIB 只要 20ms，再加 2ms 拷进 QImage。
//    41ms 意味着 15fps 下 GUI 线程有 62% 的时间在阻塞截图，界面会明显卡；
//    20ms 降到 30%，可以接受。混合 DPI 多屏时不走快路径（见 captureWholeScreenWin）。
//
// 3) **DPR 靠"抓回来的实际宽/屏幕逻辑宽"反推**，不直接读 devicePixelRatioF()。
//    只有"请求尺寸 vs 实际返回尺寸"的比值才能保证裁剪落在正确像素上。
//
// 4) **重入保护是 bool 标志，不是 QMutex::tryLock**。
//    grabWindow() / BitBlt 在 Windows 上会内部转消息泵，定时器的 timeout 可能在
//    本 tick 还没返回时又被投进来；取色器踩过这个坑（嵌套 tick 并发改写隐式共享
//    对象的引用计数 → 堆损坏 0xC0000374）。重入是**同一线程**的事件重入，不是并发，
//    用 tryLock 语义上是"抢占"而非"重入"，将来若把抓屏挪进工作线程还会静默丢帧。
// ---------------------------------------------------------------------------

#include <QCursor>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace screenrec {

/// 一次抓取的描述。regionLogic 是**相对该屏左上角**的逻辑像素矩形；
/// 留空表示整屏（更稳妥，面板默认就是整屏）。
struct Target
{
    QPointer<QScreen> screen;
    QRect regionLogic;
};

/// 抓取结果。image 的 devicePixelRatio 已被归一成 1.0（后续绘制光标、
/// 判断坐标都按设备像素，不再受 DPR 二次缩放干扰）。
struct GrabResult
{
    QImage image;
    QPoint cursorInFrame;        ///< 光标在帧内的设备像素坐标
    bool cursorInside = false;   ///< 光标是否落在录制区域内
    bool ok = false;
    bool busy = false;           ///< 因重入被跳过（不是错误，只需丢这一帧）
    QString error;               ///< 失败原因（可读中文）
};

namespace detail {

/// 抓屏重入标志。抓屏只在 GUI 线程发生，一个裸 bool 就够。
inline bool &grabbingFlag()
{
    static bool flag = false;
    return flag;
}

/// RAII 复位：任何提前 return / 异常都能把标志恢复，不会把后续帧永久挡死。
struct GrabbingGuard
{
    bool &flag;
    explicit GrabbingGuard(bool &f) : flag(f) { flag = true; }
    ~GrabbingGuard() { flag = false; }
    GrabbingGuard(const GrabbingGuard &) = delete;
    GrabbingGuard &operator=(const GrabbingGuard &) = delete;
};

} // namespace detail

/// 抓不到像素时的平台专属提示。三平台都给齐，不留空分支（AGENTS 规则 3）。
inline QString grabFailureHint()
{
#if defined(Q_OS_WIN)
    return QStringLiteral(
        "屏幕抓取失败：若处于远程桌面会话、或要录的内容受 DRM 保护，系统可能不提供像素");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(
        "屏幕抓取失败：请在「系统设置 → 隐私与安全性 → 屏幕录制」中允许本应用，"
        "未授权时系统只会返回黑屏");
#elif defined(Q_OS_LINUX)
    return QStringLiteral(
        "屏幕抓取失败：当前会话可能不允许抓取屏幕（Wayland 下常见），请改用 X11 会话登录");
#else
    return QStringLiteral("当前平台未提供屏幕抓取能力");
#endif
}

#if defined(Q_OS_WIN)

///
/// 所有屏幕的 DPR 是否一致。
///
/// 为什么必须一致才敢用 BitBlt 快路径：GetDC(NULL) 拿到的是整个虚拟桌面的 DC，
/// 它的坐标空间在 per-monitor DPI aware 进程里按各屏自己的 DPI 缩放。混合 DPI 时
/// "逻辑坐标 × 本屏 DPR" 算不出真实的虚拟桌面物理坐标，会抓错区域。这种机器不多，
/// 遇到了就退回 Qt 的 grabWindow（慢一倍但按屏处理、行为正确）。
///
inline bool allScreensShareDpr()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.size() <= 1)
        return true;
    const qreal first = screens.first()->devicePixelRatio();
    for (QScreen *screen : screens) {
        if (screen && !qFuzzyCompare(screen->devicePixelRatio(), first))
            return false;
    }
    return true;
}

///
/// 用 GDI 直接抓一整屏。成功时 out 是物理像素、devicePixelRatio 已归一为 1.0。
///
/// 失败（DC/DIBSection 创建不了、BitBlt 返回 0）时返回 false，调用方退回 Qt 实现。
/// 这里不写失败文案：真正的错误提示由 Qt 路径统一给出，避免两条路径的文案不一致。
///
inline bool captureWholeScreenWin(QScreen *screen, QImage *out)
{
    if (!screen || !out)
        return false;

    const qreal dpr = screen->devicePixelRatio();
    const QRect geometry = screen->geometry();
    const int width = qRound(geometry.width() * dpr);
    const int height = qRound(geometry.height() * dpr);
    if (width <= 0 || height <= 0)
        return false;

    // 虚拟桌面物理坐标：DPR 一致的前提下，逻辑坐标乘 DPR 就是物理坐标
    const int srcX = qRound(geometry.x() * dpr);
    const int srcY = qRound(geometry.y() * dpr);

    HDC screenDc = ::GetDC(nullptr);
    if (!screenDc)
        return false;
    HDC memDc = ::CreateCompatibleDC(screenDc);
    if (!memDc) {
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;   // 负数 = 自上而下，与 QImage 行序一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(memDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        ::DeleteDC(memDc);
        ::ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ oldBitmap = ::SelectObject(memDc, dib);
    // CAPTUREBLT：把分层窗口（部分游戏/播放器的覆盖层）也抓进来。
    const BOOL blt = ::BitBlt(memDc, 0, 0, width, height, screenDc, srcX, srcY,
                              SRCCOPY | CAPTUREBLT);
    if (blt) {
        // DIB 的像素缓冲区在 DeleteObject 之后就失效，必须立刻拷进 QImage 自己的内存。
        // GDI 不写 alpha 字节，所以这里用 RGB32（忽略 alpha）而不是 ARGB32。
        const QImage view(static_cast<const uchar *>(bits), width, height, width * 4,
                          QImage::Format_RGB32);
        *out = view.copy();
        out->setDevicePixelRatio(1.0);
    }

    ::SelectObject(memDc, oldBitmap);
    ::DeleteObject(dib);
    ::DeleteDC(memDc);
    ::ReleaseDC(nullptr, screenDc);

    return blt != 0 && !out->isNull();
}

#endif // Q_OS_WIN

///
/// 抓一帧。
///
/// @param target     目标屏幕与区域（区域是逻辑像素，相对该屏左上角）
/// @param wantCursor 是否需要光标位置（不需要时省一次 QCursor::pos）
///
/// 必须在 GUI 线程调用。失败时 ok=false 且 error 为可读中文；因重入被跳过时
/// busy=true、error 为空（这是正常的高频现象，不该当成故障上报）。
///
inline GrabResult grab(const Target &target, bool wantCursor)
{
    GrabResult result;

    QScreen *screen = target.screen.data();
    if (!screen) {
        result.error = QStringLiteral("录制目标屏幕已断开");
        return result;
    }

    bool &flag = detail::grabbingFlag();
    if (flag) {
        result.busy = true;
        return result;
    }
    detail::GrabbingGuard guard(flag);

    // ---- 抓整屏：优先平台快路径，失败或平台不支持时退回 Qt
    QImage frame;
#if defined(Q_OS_WIN)
    if (allScreensShareDpr())
        captureWholeScreenWin(screen, &frame);
#endif

    if (frame.isNull()) {
        const QPixmap shot = screen->grabWindow(0);
        if (shot.isNull()) {
            result.error = grabFailureHint();
            return result;
        }
        frame = shot.toImage();
        if (frame.isNull()) {
            result.error = grabFailureHint();
            return result;
        }
    }

    const QRect geometry = screen->geometry();
    const qreal dpr = geometry.width() > 0
                          ? qMax(1.0, qreal(frame.width()) / qreal(geometry.width()))
                          : 1.0;

    // 区域：留空 = 整屏；越界部分裁掉（用户拖到屏幕边缘时会出现）
    QRect region = target.regionLogic;
    if (region.isEmpty())
        region = QRect(0, 0, geometry.width(), geometry.height());
    region = region.intersected(QRect(0, 0, geometry.width(), geometry.height()));
    if (region.width() < 2 || region.height() < 2) {
        result.error = QStringLiteral("录制区域过小（至少需要 2×2）");
        return result;
    }

    const QRect deviceRect(qRound(region.x() * dpr),
                           qRound(region.y() * dpr),
                           qRound(region.width() * dpr),
                           qRound(region.height() * dpr));
    const QRect clipped = deviceRect.intersected(frame.rect());
    if (clipped.width() < 2 || clipped.height() < 2) {
        result.error = QStringLiteral("录制区域超出屏幕范围");
        return result;
    }

    if (clipped != frame.rect())
        frame = frame.copy(clipped);

    // 归一 DPR：后面所有坐标（光标位置、帧尺寸）都按设备像素算，
    // 保留非 1 的 DPR 会让 QPainter 在绘制光标时再缩放一次，位置就歪了。
    frame.setDevicePixelRatio(1.0);

    if (wantCursor) {
        const QPoint globalPos = QCursor::pos();
        const QPoint localInScreen = globalPos - geometry.topLeft();
        const QPoint inFrame(qRound(localInScreen.x() * dpr) - clipped.x(),
                             qRound(localInScreen.y() * dpr) - clipped.y());
        result.cursorInFrame = inFrame;
        result.cursorInside = QRect(0, 0, frame.width(), frame.height()).contains(inFrame);
    }

    result.image = frame;
    result.ok = true;
    return result;
}

/// 屏幕的物理像素尺寸（面板用它判断"是否该自动降帧率"）。
/// 拿不到时退化成逻辑尺寸，宁可少降一次也不返回 0。
inline QSize devicePixelSize(QScreen *screen)
{
    if (!screen)
        return QSize();
    const QSize logic = screen->geometry().size();
    const qreal dpr = screen->devicePixelRatio();
    if (dpr <= 1.0)
        return logic;
    return QSize(qRound(logic.width() * dpr), qRound(logic.height() * dpr));
}

} // namespace screenrec

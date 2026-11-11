#pragma once

// ---------------------------------------------------------------------------
// 把鼠标光标画进录制的画面（纯 header，只依赖 QtGui）。
//
// 为什么必须自己做：QScreen::grabWindow() 抓不到光标（它抓的是窗口内容，
// 光标是系统合成层），而 QCursor::pixmap() 在 Qt 6 对**系统标准光标**返回空图
// （只有用 QPixmap 显式构造的 QCursor 才有内容），所以两者都不能直接用。
//
// 策略：
//   * Windows：GetCursorInfo + GetIconInfo 拿真实的当前光标位图。要处理两种形态：
//       - 彩色光标（hbmColor 非空，32bpp 带 alpha）—— 现代默认光标；
//       - 单色光标（hbmColor 为空，只有 AND/XOR 掩码位图）—— 精简安装、虚拟机、
//         远程会话里很常见，**不能跳过**（本机就是这种）。
//   * 其它平台（以及上面的调用失败时）：画一个自绘箭头兜底。
//
// 名称约定：image 与 hotspot 都是**设备像素**，因为调用方（RecordService）传进来的
// 帧已经被归一到 devicePixelRatio == 1.0。
// ---------------------------------------------------------------------------

#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QPolygonF>
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

struct CursorBitmap
{
    QImage image;          ///< ARGB32_Premultiplied，设备像素；空表示取不到
    QPoint hotspot;        ///< 热点的设备像素坐标（左上为 0,0）
};

///
/// 自绘光标箭头，给非 Windows 平台和"取不到系统位图"的情况兜底。
///
/// **这里的黑白是字面量，不走主题令牌**（AGENTS 规则 7 的例外）：光标要压在
/// 任意颜色的画面内容上，跟着主题变深会在深色背景里彻底消失、变浅会在白底上消失。
/// 白色填充 + 深色描边是唯一在深浅背景上都能看清的组合。
///
inline QImage fallbackCursorImage()
{
    static constexpr int kW = 12;
    static constexpr int kH = 18;

    // 建一次缓存复用：每帧重建 12×18 的图虽然便宜，但没必要。
    static const QImage cached = [] {
        QImage image(kW, kH, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        QPolygonF arrow;
        arrow << QPointF(0.8, 0.8)
              << QPointF(0.8, 15.2)
              << QPointF(4.2, 11.9)
              << QPointF(6.9, 17.4)
              << QPointF(9.0, 16.4)
              << QPointF(6.4, 10.9)
              << QPointF(11.0, 10.7);

        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(18, 18, 18), 1.3));
        painter.setBrush(QColor(255, 255, 255));
        painter.drawPolygon(arrow);
        return image;
    }();
    return cached;
}

/// 自绘箭头的热点位置（箭头尖）。
inline QPoint fallbackCursorHotspot()
{
    return QPoint(1, 1);
}

namespace detail {

#if defined(Q_OS_WIN)

/// 把位图按 32bpp 读进 ARGB32 QImage。失败返回空图。
/// GetDIBits 给出的 1bpp 会被展开成 0xFFFFFF（白=1）与 0（黑=0）。
inline QImage bitmapToArgb(HBITMAP bitmap, int width, int height, bool topDown = true)
{
    if (!bitmap || width <= 0 || height <= 0)
        return {};

    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::transparent);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = topDown ? -height : height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!dc)
        return {};
    const int scanned = ::GetDIBits(dc, bitmap, 0, UINT(height), image.bits(), &bi, DIB_RGB_COLORS);
    ::DeleteDC(dc);

    if (scanned != height)
        return {};
    return image;
}

/// 单色光标合成：掩码位图高度是光标的两倍，上半是 AND、下半是 XOR。
///    AND=1, XOR=0 → 透明（保留桌面原样）
///    AND=0, XOR=0 → 黑色
///    AND=0, XOR=1 → 白色
///    AND=1, XOR=1 → 反色（录屏无法反色，这里近似成白色）
inline QImage composeMonoCursor(const QImage &maskBits, int width, int height)
{
    if (maskBits.isNull() || maskBits.height() < height * 2)
        return {};

    QImage out(width, height, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);

    for (int y = 0; y < height; ++y) {
        const uchar *andRow = maskBits.constScanLine(y);
        const uchar *xorRow = maskBits.constScanLine(y + height);
        auto *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; ++x) {
            // 只看 B 通道：1bpp 展开后非白即黑
            const bool andBit = andRow[x * 4] != 0;
            const bool xorBit = xorRow[x * 4] != 0;
            if (andBit && !xorBit)
                dst[x] = 0x00000000;   // 透明
            else if (!andBit && !xorBit)
                dst[x] = 0xFF000000;   // 黑
            else
                dst[x] = 0xFFFFFFFF;   // 白
        }
    }
    return out;
}

#endif // Q_OS_WIN

} // namespace detail

///
/// 取当前系统光标位图。取不到（没有光标、非 Windows 平台）时返回 image 为空的
/// CursorBitmap，调用方改用 fallbackCursorImage()。
///
/// 每帧调用的开销很小（GetCursorInfo/GetIconInfo/GetDIBits 都在微秒级，光标位图
/// 只有 32×32），但会产生 GDI 位图，必须在函数内及时 DeleteObject。
///
inline CursorBitmap currentCursorBitmap()
{
#if defined(Q_OS_WIN)
    CURSORINFO ci{};
    ci.cbSize = sizeof(CURSORINFO);
    if (!::GetCursorInfo(&ci) || (ci.flags & CURSOR_SHOWING) == 0 || ci.hCursor == nullptr)
        return {};

    ICONINFO info{};
    if (!::GetIconInfo(ci.hCursor, &info))
        return {};

    // GetIconInfo 会把两个位图**复制**出来，调用方负责删除。
    // 漏删的后果不是"慢一点"：GDI 句柄上限 10000，15fps 下几十秒就耗尽，
    // 之后整个桌面都画不出来。用 RAII 保证每条 return 路径都清理。
    struct BitmapReleaser
    {
        HBITMAP color = nullptr;
        HBITMAP mask = nullptr;
        ~BitmapReleaser()
        {
            if (color)
                ::DeleteObject(color);
            if (mask)
                ::DeleteObject(mask);
        }
    } releaser{info.hbmColor, info.hbmMask};

    CursorBitmap out;
    out.hotspot = QPoint(int(info.xHotspot), int(info.yHotspot));

    if (info.hbmColor != nullptr) {
        // ---- 彩色光标：32bpp 自带 alpha
        BITMAP bm{};
        if (!::GetObject(info.hbmColor, sizeof(bm), &bm) || bm.bmBitsPixel != 32)
            return {};
        const int width = bm.bmWidth;
        const int height = bm.bmHeight;
        if (width <= 0 || height <= 0 || width > 512 || height > 512)
            return {};

        QImage image = detail::bitmapToArgb(info.hbmColor, width, height);
        if (image.isNull())
            return {};

        // 老式 .cur 用掩码定义形状，彩色位图里 alpha 可能全 0。直接用会让光标
        // 彻底消失，所以退回去用掩码补一次（掩码白 = 透明）。
        bool anyAlpha = false;
        const qsizetype bytes = qsizetype(width) * height * 4;
        {
            const uchar *bits = image.constBits();
            for (qsizetype i = 3; i < bytes; i += 4) {
                if (bits[i] != 0) {
                    anyAlpha = true;
                    break;
                }
            }
        }
        if (!anyAlpha && info.hbmMask != nullptr) {
            BITMAP mb{};
            if (::GetObject(info.hbmMask, sizeof(mb), &mb) && mb.bmHeight >= height) {
                const QImage maskBits = detail::bitmapToArgb(info.hbmMask, width, mb.bmHeight);
                if (!maskBits.isNull()) {
                    uchar *dst = image.bits();
                    for (int y = 0; y < height; ++y) {
                        const uchar *src = maskBits.constScanLine(y);
                        for (int x = 0; x < width; ++x)
                            dst[qsizetype(y) * width * 4 + x * 4 + 3] = src[x * 4] != 0 ? 0 : 255;
                    }
                }
            }
        }

        out.image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        return out;
    }

    if (info.hbmMask != nullptr) {
        // ---- 单色光标：只有 AND/XOR 掩码，高度是光标的两倍。
        // 这条路径在精简安装、虚拟机、远程会话里很常见（本机就是），不能省。
        BITMAP mb{};
        if (!::GetObject(info.hbmMask, sizeof(mb), &mb))
            return {};
        const int width = mb.bmWidth;
        const int height = mb.bmHeight / 2;
        if (width <= 0 || height <= 0 || width > 512 || height > 512)
            return {};

        const QImage maskBits = detail::bitmapToArgb(info.hbmMask, width, mb.bmHeight);
        if (maskBits.isNull())
            return {};

        out.image = detail::composeMonoCursor(maskBits, width, height);
        return out;
    }

    return {};
#else
    // macOS / Linux：没有既统一又稳定的 API 能拿"当前系统光标位图"
    // （macOS 要 NSCursor 私有接口，X11 要 XFixesGetCursorImage，Wayland 完全不给）。
    // 宁可画一个统一的自绘箭头，也不写三段无法实机验证的原生代码。
    return {};
#endif
}

///
/// 把光标画到帧上。pos 是光标热点在帧内的设备像素坐标。
///
/// 越界时不做裁剪：光标压在录制区域边缘时，露一半比直接消失更符合直觉
/// （QPainter 自己会裁掉帧外的部分）。
///
inline void drawCursor(QImage *frame, const CursorBitmap &bitmap, const QPoint &pos)
{
    if (frame == nullptr || frame->isNull())
        return;

    const bool useFallback = bitmap.image.isNull();
    const QImage &cursor = useFallback ? fallbackCursorImage() : bitmap.image;
    const QPoint hotspot = useFallback ? fallbackCursorHotspot() : bitmap.hotspot;

    QPainter painter(frame);
    // 光标是像素级的界面元素，插值会让箭头边缘糊成一团
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(pos - hotspot, cursor);
}

} // namespace screenrec

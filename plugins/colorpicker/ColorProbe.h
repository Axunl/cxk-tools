#pragma once

// ---------------------------------------------------------------------------
// 屏幕取样层。只依赖 Qt 的跨平台 API（QScreen::grabWindow），不写任何原生调用，
// 因此三平台共用一份实现；平台差异全部收敛成「可读的能力/授权说明」。
//
// 为什么不写原生取色代码：Windows GetPixel / macOS CGWindowListCreateImage /
// X11 XGetImage 三套语义、坐标原点、授权模型都不同，在没有对应实机的情况下
// 写出来就是未经验证的代码（AGENTS.md 规则 3 明确禁止）。Qt 的 grabWindow()
// 在三端都有实现，失败时由本文件给出明确原因，绝不静默返回黑色。
// ---------------------------------------------------------------------------

#include <QColor>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QtGlobal>

namespace colorprobe {

/// 一次取样的结果。ok=false 时 error 一定非空，由面板显示给用户。
struct Sample
{
    QColor color;              ///< 光标处的像素（统一为不透明）
    QImage around;             ///< 邻域原图，给放大镜用（设备像素，可能已按 DPR 放大）
    QPoint  pos;               ///< 取样时的全局光标位置
    bool    ok = false;
    QString error;
};

/// 平台相关的能力说明。
///
/// 这段是三平台分支齐整的「行为解释」而非原生实现：
///  - macOS 10.15+ 抓屏需要「屏幕录制」授权，未授权时系统返回全黑画面（Qt 不会报错），
///    所以必须把这层限制提前告诉用户，否则用户会以为吸管坏了；
///  - Linux Wayland 不提供跨窗口读像素的接口，X11 会话才可用；
///  - Windows 正常，但远程桌面 / 受保护窗口可能拿到黑屏；
///  - 其余平台一律明确说不支持，不假装成功。
inline QString platformHint()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows 下可直接抓取屏幕；若始终取到黑色，请检查是否处于远程桌面或受保护窗口。");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Linux 需要 X11 会话：Wayland 不允许应用读取其他窗口像素，取色可能失败；此时请用手动输入或从剪贴板读入。");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macOS 需在「系统设置 → 隐私与安全性 → 屏幕录制」中允许本应用；未授权时系统只返回黑屏，请改用手动输入或从剪贴板读入。");
#else
    return QStringLiteral("当前平台未提供屏幕取色能力，请使用手动输入或从剪贴板读入颜色。");
#endif
}

/// 抓取光标周围 (2*radius+1) 见方的屏幕区域，并给出中心像素颜色。
///
/// @param globalPos QCursor::pos() 的全局坐标（Qt 的逻辑像素坐标系）
/// @param radius    放大镜半径（逻辑像素），边缘处会自动裁剪到屏幕内
///
/// 高 DPI 处理：不假设 grabWindow() 的 x/y 是逻辑像素还是设备像素，也不假设
/// 返回图与请求区 1:1 —— 用「返回图尺寸 / 请求区尺寸」的实际比例把光标位置
/// 映射回图像坐标，再取中心像素。这样 DPR=1/2、图像多返回一行一列时都不会
/// 取错点；放大镜则直接显示这张原图，天然保持正确的像素密度。
inline Sample grabAt(const QPoint &globalPos, int radius = 6)
{
    Sample sample;
    sample.pos = globalPos;

    // 纯逻辑的临时测试工程不会构造 QGuiApplication，此时直接给可读失败原因，
    // 不去 dereference 空实例（注册表可能还有别的 QCoreApplication 子类）。
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        sample.error = QStringLiteral("当前没有图形应用实例，无法读取屏幕");
        return sample;
    }

    QScreen *screen = QGuiApplication::screenAt(globalPos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen) {
        sample.error = QStringLiteral("找不到光标所在的屏幕；") + platformHint();
        return sample;
    }

    const QRect screenArea(QPoint(0, 0), screen->size());
    const QPoint local = globalPos - screen->geometry().topLeft();
    const QRect wanted(local.x() - radius, local.y() - radius, 2 * radius + 1, 2 * radius + 1);
    const QRect area = wanted.intersected(screenArea);   // 光标贴边时不要向屏幕外抓
    if (area.isEmpty()) {
        sample.error = QStringLiteral("光标不在任何屏幕范围内");
        return sample;
    }

    const QPixmap pixmap = screen->grabWindow(0, area.x(), area.y(), area.width(), area.height());
    if (pixmap.isNull()) {
        sample.error = QStringLiteral("屏幕像素读取失败；") + platformHint();
        return sample;
    }

    QImage image = pixmap.toImage();
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        sample.error = QStringLiteral("屏幕像素为空；") + platformHint();
        return sample;
    }

    const qreal sx = qreal(image.width()) / area.width();
    const qreal sy = qreal(image.height()) / area.height();
    const int cx = qBound(0, qRound((local.x() - area.x() + 0.5) * sx - 0.5), image.width() - 1);
    const int cy = qBound(0, qRound((local.y() - area.y() + 0.5) * sy - 0.5), image.height() - 1);

    sample.color = image.pixelColor(cx, cy);
    // 屏幕上看到的是合成后的不透明像素；个别平台返回的图带 alpha 通道，
    // 统一抹成 255，避免把偶然的透明标记当成「用户选了半透明颜色」。
    sample.color.setAlpha(255);
    sample.around = image;
    sample.ok = true;
    return sample;
}

} // namespace colorprobe

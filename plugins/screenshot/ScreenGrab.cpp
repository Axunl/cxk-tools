#include "ScreenGrab.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QtGlobal>

namespace screengrab {

namespace {

/// 把 QPixmap 转成「设备像素 + dpr=1」的 QImage。
/// toImage() 会保留 dpr，若不抹掉，后续 copy()/drawImage() 会按 dpr 再次缩放，
/// 拼接时行数就对不上了。
QImage toDeviceImage(const QPixmap &pixmap)
{
    QImage image = pixmap.toImage();
    if (!image.isNull())
        image.setDevicePixelRatio(1.0);
    return image;
}

/// 是否有可用的图形应用实例。临时验证工程只会构造 QCoreApplication，
/// 这时不能去 dereference QGuiApplication 的单例指针。
bool hasGraphicalApp()
{
    return qobject_cast<QGuiApplication *>(QCoreApplication::instance()) != nullptr;
}

} // namespace

QImage Snapshot::compose() const
{
    if (virtualGeometry.isEmpty())
        return {};

    QImage canvas(virtualGeometry.size(), QImage::Format_RGB32);
    canvas.fill(Qt::black);

    QPainter painter(&canvas);
    // 关掉平滑：底图只用于显示，关掉后缩放更快，也不再额外引入一遍插值模糊。
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    for (const ScreenShot &shot : screens) {
        if (shot.devicePixels.isNull())
            continue;
        // 目标矩形用逻辑坐标（相对虚拟桌面左上角），源图是设备像素；
        // drawImage 会按目标矩形自动缩放，正好把各屏不同的 DPR 抹平。
        const QRect target = shot.logicalGeometry.translated(-virtualGeometry.topLeft());
        painter.drawImage(target, shot.devicePixels);
    }
    return canvas;
}

int Snapshot::screenIndexAt(const QPoint &globalPoint) const
{
    for (int i = 0; i < screens.size(); ++i) {
        if (screens.at(i).logicalGeometry.contains(globalPoint))
            return i;
    }
    return -1;
}

QImage Snapshot::crop(const QRect &logicalRegion, qreal *outDpr) const
{
    if (logicalRegion.isEmpty())
        return {};

    // 以左上角判定归属屏幕：与遮罩里「选区不得跨屏」的约束配套
    // （跨屏时选区会被夹回起始屏幕，见 CaptureOverlay::clampToScreen）。
    const int index = screenIndexAt(logicalRegion.topLeft());
    if (index < 0)
        return {};

    const ScreenShot &shot = screens.at(index);
    if (shot.devicePixels.isNull())
        return {};

    const QRect local = logicalRegion.translated(-shot.logicalGeometry.topLeft());
    QRect device(qRound(local.x() * shot.dpr), qRound(local.y() * shot.dpr),
                 qRound(local.width() * shot.dpr), qRound(local.height() * shot.dpr));
    device = device.intersected(shot.devicePixels.rect());
    if (device.isEmpty())
        return {};

    QImage cropped = shot.devicePixels.copy(device);
    if (outDpr)
        *outDpr = shot.dpr;
    // 设回缩放比：这样界面里按逻辑尺寸显示、存成 PNG 后按物理尺寸显示，
    // 而不是被当成 1/2 大小的一张图。
    cropped.setDevicePixelRatio(shot.dpr);
    return cropped;
}

Snapshot captureAll()
{
    Snapshot snapshot;
    if (!hasGraphicalApp())
        return snapshot;

    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (!screen)
            continue;
        const QRect geometry = screen->geometry();
        if (geometry.isEmpty())
            continue;

        const QPixmap pixmap = screen->grabWindow(0);
        if (pixmap.isNull())   // 单块屏幕抓不到（授权/剪裁），跳过，不拖累其它屏
            continue;

        QImage image = toDeviceImage(pixmap);
        if (image.isNull() || image.width() <= 0 || image.height() <= 0)
            continue;

        // 用「返回图宽 / 逻辑宽」反推缩放比，而不是直接读 screen->devicePixelRatio()：
        // 有的平台返回逻辑像素图，两者会不一致，写死就会裁歪。
        const qreal dpr = qMax(1.0, qreal(image.width()) / qreal(geometry.width()));

        ScreenShot shot;
        shot.logicalGeometry = geometry;
        shot.devicePixels = image;
        shot.dpr = dpr;
        snapshot.screens.push_back(shot);
        snapshot.virtualGeometry = snapshot.virtualGeometry.united(geometry);
    }
    return snapshot;
}

QImage grabRegion(const QRect &logicalRegion, qreal *outDpr)
{
    if (!hasGraphicalApp() || logicalRegion.isEmpty())
        return {};

    QScreen *screen = QGuiApplication::screenAt(logicalRegion.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return {};

    const QRect screenGeometry = screen->geometry();
    // 夹在屏幕内：滚动截图期间用户可能切换窗口，选区越界会让每帧尺寸不一致，
    // 直接导致拼接失败，所以宁可裁掉越界部分（选区本来就被遮罩限制在单屏内）。
    const QRect local = logicalRegion.translated(-screenGeometry.topLeft())
                            .intersected(QRect(QPoint(0, 0), screenGeometry.size()));
    if (local.isEmpty())
        return {};

    const QPixmap pixmap = screen->grabWindow(0, local.x(), local.y(), local.width(), local.height());
    if (pixmap.isNull())
        return {};

    QImage image = toDeviceImage(pixmap);
    if (image.isNull() || image.width() <= 0)
        return {};

    // 缩放比按「实际返回宽 / 请求逻辑宽」算，与 captureAll() 同一套口径。
    // 这里刻意不做归一化缩放：同一块区域每帧都会走同一段平台代码，返回尺寸
    // 天然一致，额外缩放只会引入插值误差，反而破坏逐行匹配。
    if (outDpr)
        *outDpr = qMax(1.0, qreal(image.width()) / qreal(local.width()));
    return image;
}

QString platformHint()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows 下应能直接抓屏；若始终失败，请检查是否运行在"
                          "受保护的桌面（UAC 提示、远程桌面独占会话）中。");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Linux 需要 X11 会话：Wayland 不允许应用读取其他窗口的像素，"
                          "请在登录界面切换到「Xorg / X11」会话后重试。");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macOS 需在「系统设置 → 隐私与安全性 → 屏幕录制」里允许本应用；"
                          "未授权时系统只返回黑屏或空图。");
#else
    return QStringLiteral("当前平台未提供屏幕抓取能力。");
#endif
}

} // namespace screengrab

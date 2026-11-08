#pragma once

// ---------------------------------------------------------------------------
// 窗口几何的保存 / 恢复（主窗口与独立工具窗口共用）。
//
// 抽成公共头文件而不是各写一遍：两处的规则必须一致 —— 几何串可能因为换显示器、
// 改分辨率而失效，此时必须退回默认尺寸并居中，否则窗口会落在屏幕外「消失」，
// 用户只能靠任务栏或热键找回来。只用 QGuiApplication::screens()，不碰平台 API。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QWidget>

namespace WindowGeometry {

/// 窗口中心点是否落在某块屏幕的可用区域内（有交集就算，避免贴边窗口被误判）。
inline bool isReachable(const QRect &frameRect)
{
    const QPoint center = frameRect.center();
    const auto screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        if (screen->availableGeometry().contains(center))
            return true;
    }
    return false;
}

/// 按默认尺寸居中到主屏可用区域。
inline void centerOnPrimaryScreen(QWidget *w, const QSize &fallbackSize)
{
    w->resize(fallbackSize);
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        w->move(avail.center() - QPoint(w->width() / 2, w->height() / 2));
    }
}

/// 恢复几何。geo 为空 / 解析失败 / 恢复后跑到屏幕外，都退回默认尺寸并居中。
/// @return 是否真的用上了保存的几何（调用方可据此决定要不要再摆位置）。
inline bool restore(QWidget *w, const QByteArray &geo, const QSize &fallbackSize)
{
    if (!geo.isEmpty() && w->restoreGeometry(geo) && isReachable(w->frameGeometry()))
        return true;
    centerOnPrimaryScreen(w, fallbackSize);
    return false;
}

} // namespace WindowGeometry

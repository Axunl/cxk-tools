#pragma once

// ---------------------------------------------------------------------------
// 屏幕抓取层：整屏冻结快照、任意区域重新抓取、从快照里裁剪。
//
// 只依赖 Qt 的跨平台 API（QScreen::grabWindow），不写任何原生调用 —— 三端行为
// 交给 Qt 统一保证。这与 AGENTS.md 规则 3 的要求一致：确实做不到的能力才写平台
// 分支，并且只用来给出「为什么做不到」的可读文案（见 platformHint）。
//
// 高 DPI 的约定（整个插件最关键的一条）：
//   Qt 文档只说 grabWindow()「返回的图可能比请求的大」，没有保证一定按
//   devicePixelRatio 放大，各平台实现历史上并不一致。所以这里**不假设**平台行为，
//   而是用「实际返回尺寸 / 请求的逻辑尺寸」反推缩放比；再把图统一归一成
//   「设备像素 + dpr=1」的内部表示。
//   原因：拼接时要逐像素比较两帧、按行搬运像素，只有所有帧处在同一坐标系里
//   才不会出现「一帧被隐式缩放成 1/2 大小」的错位。dpr 只在最后交给界面的那一刻
//   设回去，让预览与保存出来的图按物理尺寸显示。
// ---------------------------------------------------------------------------

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace screengrab {

/// 一块屏幕的冻结截图。
struct ScreenShot
{
    QRect  logicalGeometry;   ///< 该屏幕在「虚拟桌面」里的逻辑坐标（多屏时可能为负）
    QImage devicePixels;      ///< 设备像素原图，dpr 已归一为 1
    qreal  dpr = 1.0;         ///< 该屏的实际缩放比（由返回图反推）
};

/// 所有屏幕的一次性冻结快照。滚动截图期间用它做底图，保证「用户框选时看到的」
/// 与「最终裁出来的」是同一份像素。
struct Snapshot
{
    QVector<ScreenShot> screens;
    QRect virtualGeometry;    ///< 各屏逻辑几何的并集，即遮罩要覆盖的整块区域

    bool isValid() const { return !screens.isEmpty() && !virtualGeometry.isEmpty(); }

    /// 把所有屏幕按逻辑尺寸拼成一张整图（dpr=1）。只给遮罩当底图用：
    /// 遮罩的作用是让用户看清并框选，轻微缩放不影响判断；最终裁剪走 crop()，
    /// 走的是各屏自己的设备像素，不经过这次合成。
    QImage compose() const;

    /// 全局逻辑点落在第几块屏幕里（按矩形包含判断），找不到返回 -1。
    int screenIndexAt(const QPoint &globalPoint) const;

    /// 从快照裁一块（全局逻辑坐标）。返回值是设备像素，dpr 设成该屏的缩放比；
    /// outDpr 回传同一个值，供调用方在保存/拼图时沿用。
    QImage crop(const QRect &logicalRegion, qreal *outDpr = nullptr) const;
};

/// 抓取当前所有屏幕。抓不到的屏幕（Wayland、未授权、受保护内容）会被跳过；
/// 全部失败时返回 isValid()==false 的快照，由调用方给可读原因。
Snapshot captureAll();

/// 重新抓一块区域（滚动截图的每一帧）。返回值是设备像素、dpr=1，
/// 同一块区域多次抓取得到的尺寸必然一致 —— 这是逐行拼接的前提。
/// 失败返回空图。
QImage grabRegion(const QRect &logicalRegion, qreal *outDpr = nullptr);

/// 抓不到屏幕时的原因说明。平台分支在这里只是文案，不是原生实现。
QString platformHint();

} // namespace screengrab

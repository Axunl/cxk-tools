#pragma once

// ---------------------------------------------------------------------------
// 全屏框选截图浮层（纯 Qt，不含任何平台原生调用）。
//
// 为什么用 QWidget 自绘而不用 QScreen::grabWindow + QRubberBand：
//   * 需要把整屏截图当背景、选区外压暗、选区内保持清晰，QRubberBand 做不到；
//   * 三端的窗口层级/透明行为都由 Qt 统一处理，不必写 Win32/AppKit/X11 代码。
//
// 关键约定：
//   * 该类**没有 Q_OBJECT**：只需要回调，不需要信号槽/属性，因此不依赖 moc，
//     也能被临时验证工程直接 include；
//   * 刻意不设 parent —— 调用方要先隐藏宿主窗口再截图，若挂成宿主的子窗口，
//     宿主 hide() 时会把浮层一起藏掉。生命周期由调用方用 QPointer 兜住；
//   * 坐标一律用逻辑像素；裁剪时才乘 devicePixelRatio 回到原图分辨率，
//     这样高 DPI 屏上不会把选区截歪，也不会因为缩放损失 OCR 精度。
// ---------------------------------------------------------------------------

#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>

///
/// 一次性的框选遮罩。构造后由调用方 show()；完成/取消后自行 deleteLater()。
/// 完成回调给出裁剪后的原分辨率 QPixmap，取消回调无参数。
///
class OcrCaptureOverlay : public QWidget
{
public:
    OcrCaptureOverlay(const QRect &screenGeometry, const QPixmap &screenShot, qreal dpr,
                      std::function<void(const QPixmap &)> onCaptured,
                      std::function<void()> onCancelled);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    /// 把拖拽的起止点归一成左上-右下矩形，并夹在窗口范围内。
    QRect normalizedSelection() const;

    /// 裁剪原图并回调；矩形过小视为误点，按取消处理。
    void finishSelection();

    /// 取消（Esc / 误点 / 窗口被系统关闭）：恢复宿主可见性由调用方的回调负责。
    void cancel();

    /// 小于这个尺寸不算有效选区：单击桌面时不该触发一次空识别。
    static constexpr int kMinSelectionPx = 8;

    QPixmap m_shot;      ///< 屏幕原图（设备像素）
    QPixmap m_display;   ///< 按逻辑像素缩放后的底图，仅用于绘制
    qreal m_dpr = 1.0;
    QPoint m_start;
    QPoint m_current;
    bool m_dragging = false;
    bool m_finished = false;   ///< 防重复回调：release 与 closeEvent 可能都来一次
    std::function<void(const QPixmap &)> m_onCaptured;
    std::function<void()> m_onCancelled;
};

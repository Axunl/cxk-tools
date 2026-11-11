#pragma once

// ---------------------------------------------------------------------------
// 区域录制前的全屏框选浮层（结构照抄 plugins/ocr/OcrCapture.h）。
//
// 为什么不复用 OcrCaptureOverlay：那个类的回调签名是「裁剪后的 QPixmap」，
// 职责是"交出一块图"；而这里只需要一段矩形（逻辑像素），截图只是给用户看的底图。
// 硬套会把两个不相干的生命周期绑在一起，也让 OCR 插件多担一个不该有的依赖。
//
// 关键约定（与 OCR 一致，这两条都是踩过坑总结的）：
//   * **没有 Q_OBJECT**：只需要回调，不依赖 moc；
//   * **刻意不设 parent**：调用方若要隐藏宿主窗口，挂成子控件会被一起藏掉；
//   * 坐标全程用逻辑像素，只有在**显示尺寸提示**时才乘 DPR 换算成物理像素 ——
//     录出来的视频尺寸是物理像素，用户看到的数字必须和产物一致。
// ---------------------------------------------------------------------------

#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QWidget>

#include <functional>

class QMouseEvent;
class QKeyEvent;
class QPaintEvent;
class QCloseEvent;

namespace screenrec {

///
/// 一次性的框选遮罩。构造后由调用方 show()；完成/取消后自行 deleteLater()。
///
class RegionSelectOverlay : public QWidget
{
public:
    /// @param screenGeometry 只覆盖一块屏幕（多屏跨屏框选不做，与 OCR 保持一致的取舍）
    /// @param screenShot     该屏的静态截图，作为底图
    /// @param dpr            该屏的逻辑→物理缩放比，仅用于显示"将录成多大"
    /// @param onSelected     确认回调，参数是**逻辑像素**、相对该屏左上角
    /// @param onCancelled    取消回调
    RegionSelectOverlay(const QRect &screenGeometry, const QPixmap &screenShot, qreal dpr,
                        std::function<void(const QRect &)> onSelected,
                        std::function<void()> onCancelled);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    /// 把拖拽起止点归一成左上-右下矩形，并夹在窗口内。
    QRect normalizedSelection() const;
    /// 确认选区；矩形过小视为误点，按取消处理。
    void finishSelection();
    void cancel();

    /// 小于这个尺寸不算有效选区：单击桌面不该开始录一个 1×1 的视频。
    static constexpr int kMinSelectionPx = 16;

    QPixmap m_shot;
    qreal m_dpr = 1.0;
    QPoint m_start;
    QPoint m_current;
    bool m_dragging = false;
    bool m_finished = false;   ///< 防重复回调：release 与 closeEvent 可能都来一次
    std::function<void(const QRect &)> m_onSelected;
    std::function<void()> m_onCancelled;
};

} // namespace screenrec

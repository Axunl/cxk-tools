#pragma once

// ---------------------------------------------------------------------------
// 全屏框选遮罩（纯 Qt 自绘，无任何平台原生调用）。
//
// 与 ocr 插件的 OcrCaptureOverlay 是两套实现，刻意不共用：
//   * 那个只覆盖光标所在的一块屏，且只回调裁剪后的图片；
//   * 这个要覆盖**整块虚拟桌面**（多屏拼接后的区域），并且要把选区的
//     **全局逻辑坐标**交回去 —— 滚动截图需要拿同一块区域反复重新抓帧，
//     只给一张静态图是做不到的。
//
// 关键约定：
//   * 没有 Q_OBJECT：只需要回调，不需要信号槽，也就不依赖 moc，
//     可以被临时验证工程直接 include；
//   * 不设 parent：调用方要先 hide() 自己的窗口再截图，若挂成子窗口，
//     宿主一 hide 会把遮罩一起藏掉。生命周期由调用方用 QPointer 兜住；
//   * 选区**限制在单块屏幕内**：跨屏时每块屏的 DPR 可能不同，裁剪与拼接
//     都要处理两套坐标系，而收益（选区横跨两块显示器）几乎没有。拖动越界时
//     选区被夹回起始屏幕，界面上的尺寸提示也随之停在屏幕边缘。
// ---------------------------------------------------------------------------

#include "ScreenGrab.h"

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

#include <functional>

class CaptureOverlay : public QWidget
{
public:
    /// @param snapshot 抓屏时冻结下来的整屏快照（遮罩底图与最终裁剪共用它）
    /// @param tip      顶部提示文案，两种截图模式的说明不同
    /// @param onSelected 完成回调，参数是**全局逻辑坐标**下的选区
    /// @param onCancelled 取消回调（Esc / 误点 / 窗口被系统关掉）
    CaptureOverlay(const screengrab::Snapshot &snapshot,
                   const QString &tip,
                   std::function<void(const QRect &)> onSelected,
                   std::function<void()> onCancelled);

    /// 逻辑坐标 ↔ 全局坐标。遮罩窗口的几何就是虚拟桌面，两者只差一个左上角偏移。
    QPoint toGlobal(const QPoint &local) const;
    QPoint toLocal(const QPoint &global) const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    /// 拖拽起止点归一成左上-右下矩形，并夹在窗口范围内。
    QRect selectionLocal() const;

    /// 把选区夹回「拖拽起点所在的屏幕」。见文件头的跨屏说明。
    QRect clampToScreen(const QRect &localRect) const;

    /// 裁剪快照并回调全局坐标；矩形过小视为误点，按取消处理。
    void finishSelection();

    void cancel();

    /// 判断是否需要重绘（选区没变就不重绘，鼠标移动时的重绘开销能省一半）。
    void setCurrentPoint(const QPoint &local);

    /// 小于这个尺寸不算有效选区：单击桌面不该触发一次空截图。
    static constexpr int kMinSelectionPx = 8;

    QImage m_base;                     ///< 虚拟桌面合成图（逻辑尺寸，dpr=1）
    QRect  m_virtualGeometry;          ///< 全局逻辑坐标
    screengrab::Snapshot m_snapshot;   ///< 最终裁剪要用设备像素原图
    QString m_tip;

    QPoint m_start;      ///< 局部坐标
    QPoint m_current;
    bool m_dragging = false;
    bool m_finished = false;   ///< 防重复回调：release 与 closeEvent 可能都来一次

    std::function<void(const QRect &)> m_onSelected;
    std::function<void()> m_onCancelled;
};

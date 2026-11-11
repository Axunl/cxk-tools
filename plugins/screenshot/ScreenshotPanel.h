#pragma once

// ---------------------------------------------------------------------------
// 截图工具面板（插件内部类，宿主只认 ITool）。
//
// 两条链路：
//   区域截图：隐藏宿主窗口 → 冻结整屏 → 框选 → 直接裁剪 → 自动复制到剪贴板
//   滚动截图：隐藏宿主窗口 → 框选 → 用户手动滚动 → 逐帧拼接 → 自动复制
//
// 几个刻意的取舍：
//   * 截图期间宿主窗口是 hide() 的，而不是「缩到最小」：缩到最小仍会出现在
//     任务栏，而且部分窗口管理器会把它放到桌面最前，截出来带残影。
//   * 框选用的底图与最终裁剪用的是**同一份冻结快照**：遮罩里看到的像素就是
//     存下来的像素，不会因为截图期间桌面内容变化而出现「框内是 A、结果成了 B」。
//   * 面板本身不缓存任何静态状态：宿主可能同时存在内嵌面板与独立窗口面板，
//     两份互不干扰（与 ocr / colorpicker 的约定一致）。
// ---------------------------------------------------------------------------

#include "ScreenGrab.h"

#include <QImage>
#include <QPointer>
#include <QRect>
#include <QWidget>

class CaptureOverlay;
class ScrollSession;
class QLabel;
class QPushButton;
class QScrollArea;

class ScreenshotPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenshotPanel(QWidget *parent = nullptr);

    /// 浮层与会话都不设 parent（见 CaptureOverlay.h 的说明），面板销毁时必须
    /// 自己收掉，否则遮罩会留在桌面上、宿主窗口也会停在隐藏状态。
    ~ScreenshotPanel() override;

private:
    void buildUi();

    /// 发起一次截图（scrolling=true 为滚动截图）：先隐藏宿主窗口再延时抓屏，
    /// 延时是为了让桌面完成重绘，否则会截到主窗口残影。
    void startCapture(bool scrolling);
    void beginOverlay();

    /// 框选完成：区域截图直接裁剪，滚动截图转入拼接会话。
    void onRegionSelected(const QRect &region, bool scrolling);

    /// 把宿主窗口恢复到截图前的可见状态；成功/取消/异常都要调一次。
    void restoreHostWindow();

    /// 显示一张结果图。autoCopy=true 时同时写入剪贴板（截图工具的常规行为）。
    void setImage(const QImage &image, const QString &note, bool autoCopy);

    void updatePreview();
    void updateActions();

    void copyImage();
    void saveImage();
    void clearImage();

    /// error=true 会把状态文字标红（qss 的 QLabel[state="error"]），
    /// 动态属性改完必须重新 polish，否则样式不刷新（与其它插件同一套做法）。
    void setStatus(const QString &text, bool error = false);

    /// 隐藏主窗口到抓屏之间的等待时间（毫秒）。
    static constexpr int kCaptureDelayMs = 260;

    /// 长图高度上限（像素）。20000 px 高、2000 px 宽约 160 MB，
    /// 是「够用」与「别把内存吃满」之间的折中；撞到上限会在状态栏说明。
    static constexpr int kMaxScrollHeight = 20000;

    QPushButton *m_regionBtn = nullptr;
    QPushButton *m_scrollBtn = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QLabel      *m_preview = nullptr;
    QLabel      *m_status = nullptr;
    QScrollArea *m_scrollArea = nullptr;

    QImage m_image;                      ///< 当前结果（保留原 DPR）
    screengrab::Snapshot m_snapshot;     ///< 本次框选的冻结快照，用完即清

    QPointer<CaptureOverlay> m_overlay;
    QPointer<ScrollSession>  m_session;
    QPointer<QWidget>        m_hostWindow;
    bool m_hostWasVisible = false;
    bool m_capturePending = false;
    bool m_pendingScroll = false;
};

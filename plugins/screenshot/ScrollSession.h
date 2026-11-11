#pragma once

// ---------------------------------------------------------------------------
// 滚动截图会话：一块浮动的控制条 + 一个定时抓帧循环。
//
// 交互设计（为什么这么设计）：
//   滚动截图需要「目标窗口的内容动起来」。让程序自己注入滚轮事件要在三端各写
//   一套原生代码（SendInput / XTestFakeButtonEvent / CGEventCreateScrollWheelEvent），
//   而且 macOS 还要辅助功能授权、Linux Wayland 下根本做不到 —— 这与 AGENTS.md
//   规则 3「没有实机验证就不要写平台原生代码」冲突，也必然有一个平台是残的。
//   所以这里采用**用户自己滚动、程序负责逐帧拼接**的方式：
//     * 三端行为完全一致，零原生依赖、零授权；
//     * 用户对滚动手感有完全控制权（哪一段要、哪一段不要，滚动他就停）；
//     * 唯一代价是需要用户动手，界面上的提示把这一点讲清楚。
//
// 结束条件有三种，任一满足即结束：
//   1. 用户点「完成」；
//   2. 检测到内容停止变化约 2.5 秒（说明滚完了）；
//   3. 长图达到高度上限（防止把内存吃满）。
// 窗口用 WA_ShowWithoutActivating + WindowDoesNotAcceptFocus 显示：出现时不抢焦点，
// 用户不必回到目标窗口点一下就能继续用滚轮。
// ---------------------------------------------------------------------------

#include "Stitch.h"

#include <QImage>
#include <QRect>
#include <QString>
#include <QWidget>
#include <QtGlobal>

class QLabel;
class QTimer;

class ScrollSession : public QWidget
{
    Q_OBJECT

public:
    /// @param region    选区（全局逻辑坐标），整个会话反复抓这一块
    /// @param dpr       该屏缩放比，只用于最终长图的 devicePixelRatio
    /// @param maxHeight 长图高度上限（像素）
    ScrollSession(const QRect &region, qreal dpr, int maxHeight);

signals:
    /// 正常结束。limitReached=true 表示撞到高度上限，长图可能不完整。
    void finished(const QImage &image, bool limitReached);

    /// 用户取消（点「取消」/ 窗口被系统关掉）。
    void cancelled();

    /// 抓屏失败等不可恢复错误，reason 是可以直接展示给用户的中文原因。
    void failed(const QString &reason);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    /// 一次抓帧 + 位移检测 + 拼接。由定时器驱动，约 140ms 一次。
    void tick();

    /// 把当前画布合成成图片、发 finished 并结束。
    void finishNow();

    /// 用户取消：停表、发 cancelled、结束。
    void cancelNow();

    /// 不可恢复错误：停表、发 failed、结束。
    void abort(const QString &reason);

    /// 更新「已拼接 N 帧 / 高度」这行字。
    void updateCounter();

    /// 把控制条摆到不遮挡选区的位置（选区占满整屏时才允许压在选区上）。
    void placeBar();

    /// 抓帧间隔。140ms ≈ 7Hz：滚轮一格大约 100px，这个频率下单格滚动能被
    /// 抓到 2 帧以上，重叠足够做位移匹配；再快只是白烧 CPU。
    static constexpr int kIntervalMs = 140;

    /// 连续多少次「没有变化」判定为滚完了。18 × 140ms ≈ 2.5 秒。
    /// 太短会在用户停下来看内容时提前结束，太长则要干等。
    static constexpr int kStableTicks = 18;

    /// 一帧都没拼上时最多等多久（90 × 140ms ≈ 12.6 秒）：
    /// 用户可能根本没法滚动（PDF 阅读器、图片），此时按「区域截图」收尾，
    /// 而不是让控制条一直挂着。
    static constexpr int kIdleTicks = 90;

    /// 连续抓屏失败多少次放弃。抓屏偶发失败会自己恢复，不必一次就报错。
    static constexpr int kMaxGrabFailures = 6;

    QRect  m_region;
    qreal  m_dpr = 1.0;
    int    m_maxHeight = 20000;    ///< 长图高度上限（像素）
    QTimer *m_timer = nullptr;

    QImage m_last;                 ///< 上一帧（ARGB32、dpr=1），位移检测的基准
    stitch::VerticalCanvas m_canvas;

    int  m_stableTicks = 0;        ///< 连续「没变化」的帧数
    int  m_mergeCount = 0;         ///< 成功并入画布的帧数
    int  m_grabFailures = 0;       ///< 连续抓屏失败次数
    bool m_finished = false;       ///< 防重复收尾（定时器与按钮可能同时触发）

    QLabel *m_counter = nullptr;
};

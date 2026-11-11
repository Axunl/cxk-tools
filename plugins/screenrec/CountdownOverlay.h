#pragma once

// ---------------------------------------------------------------------------
// 开始录制前的全屏 3-2-1 倒计时浮层。
//
// 为什么用全屏浮层而不是在面板里显示大数字：用户点完"开始"之后，通常要在这
// 几秒里把主窗口挪开、把要录的界面摆好 —— 这时候面板里的数字根本看不见。
// 全屏浮层只在倒计时这几秒存在，既不长期挡内容，又能让用户明确知道"什么时候开始"。
//
// 关键约定：
//   * **不抢焦点**（WindowDoesNotAcceptFocus + WA_ShowWithoutActivating）。
//     抢焦点会让被录的窗口失去高亮，录出来的头几帧能看到标题栏由蓝变灰；
//     代价是收不到键盘事件，所以取消方式是"单击任意位置"而不是 Esc。
//   * 该类**没有 Q_OBJECT**：只需要回调，不依赖 moc，也能被临时工程直接 include。
//   * 生命周期由调用方用 QPointer 兜住；结束后自己 deleteLater()。
// ---------------------------------------------------------------------------

#include <QRect>
#include <QString>
#include <QWidget>

#include <functional>

class QPaintEvent;
class QMouseEvent;
class QTimer;

namespace screenrec {

///
/// 一次性倒计时遮罩。构造后调 start() 开始；完成或取消后自行销毁。
///
class CountdownOverlay : public QWidget
{
public:
    /// @param screenGeometry 要覆盖的屏幕（逻辑像素）
    /// @param seconds        倒计时档数（每档 800ms，3 档共 2.4s）
    /// @param onFinished     倒计时走完时回调（随后才真正开始抓帧）
    /// @param onCancelled    用户单击取消时回调
    CountdownOverlay(const QRect &screenGeometry, int seconds,
                     std::function<void()> onFinished,
                     std::function<void()> onCancelled);

    void start();

    /// 外部取消（RecordService 在收到 stop / 退出请求时调用）。
    /// 会触发 onCancelled 回调并销毁自己。
    void cancel();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void advance();

    /// 每档时长。1 秒太拖沓，600ms 又来不及读；800ms 是"能看清又能等"的折中。
    static constexpr int kStepMs = 800;
    /// 隐藏后到真正开录之间的延迟。窗口 hide() 之后合成层可能还有一两帧残影，
    /// 立刻抓屏会把倒计时数字拍进第一帧（OCR 的截图流程用的是同一个 260ms）。
    static constexpr int kSettleMs = 260;

    QTimer *m_timer = nullptr;
    int m_remaining = 0;
    int m_total = 0;
    bool m_finished = false;
    std::function<void()> m_onFinished;
    std::function<void()> m_onCancelled;
};

} // namespace screenrec

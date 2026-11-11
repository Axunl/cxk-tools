#pragma once

// ---------------------------------------------------------------------------
// 录制器：**插件模块级的单例**，不属于任何一个面板。
//
// 这是本插件最重要的一条设计约束。宿主的约定是「不缓存面板」：
// MainWindow::destroyToolPanel() 在回首页 / 进设置 / 切工具 / 关主窗口时都会
// deleteLater() 掉面板（src/ui/MainWindow.cpp），关窗口进托盘也是。如果录制器
// 挂在面板上，用户只要点一下首页，正在录的视频就被截断了 —— 这对录屏工具是
// 致命的，因为录制时长往往就是几分钟。
//
// 所以：
//   * 录制器是函数内 static（不挂在 qApp 名下，DLL 卸载时 qApp 若持有它会去
//     delete 一段已 unmap 的 vtable）；
//   * 面板只订阅信号、渲染状态，析构时**什么都不做**；
//   * 新面板构造时读一次 service 状态即可恢复「录制中 00:23」，因为是幂等只读。
//
// 退出路径见 .cpp 里的 aboutToQuit 说明。
// ---------------------------------------------------------------------------

#include "CursorPainter.h"
#include "ScreenGrabber.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QtGlobal>

class QScreen;
class QThread;
class QTimer;

namespace screenrec {

class AviMjpegWriter;
class FrameQueue;
class RecorderWorker;
class CountdownOverlay;

class RecordService : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Idle,        ///< 空闲，可以开始
        Countdown,   ///< 倒计时中（还没开始抓帧）
        Recording,   ///< 正在录制
        Stopping,    ///< 已停止抓帧，正在写索引收尾
        Error        ///< 上一次录制失败，需要重新开始
    };
    Q_ENUM(State)

    struct Config
    {
        enum class Mode
        {
            FullScreen,
            Region
        };

        Mode mode = Mode::FullScreen;
        QString screenName;      ///< QScreen::name()，比存裸指针稳（屏幕可能被拔）
        QRect regionLogic;       ///< Region 模式下的区域，逻辑像素、相对该屏左上角
        int fps = 15;
        int jpegQuality = 75;
        bool drawCursor = true;
        bool countdown = true;
        /// 输出目录。留空则用 defaultOutputDir()；文件名（含重名去重）由本类生成，
        /// 不让面板各写一份 —— 两份实现迟早会不一致。
        QString outputDir;
    };

    /// 供面板渲染的统计快照。全部是"读一次就定型"的值，不做实时更新。
    struct Stats
    {
        qint64 elapsedMs = 0;
        qint64 frames = 0;     ///< 已写入帧数（含补帧）
        qint64 dups = 0;       ///< 其中补的重复帧
        qint64 dropped = 0;    ///< 队列满被丢掉的帧
        qint64 skipped = 0;    ///< 因卡顿被跳过的帧
        qint64 bytes = 0;      ///< JPEG 原始字节（不含容器开销）
        double actualFps = 0.0;
        int width = 0;
        int height = 0;
        QString path;
    };

    ~RecordService() override;

    /// 模块级单例。见文件头的生命周期说明。
    static RecordService *instance();

    State state() const { return m_state; }
    qint64 elapsedMs() const;
    Stats stats() const;
    Config activeConfig() const { return m_config; }
    QString lastOutput() const { return m_lastOutput; }
    QString lastError() const { return m_lastError; }

    /// 开始录制。已在录制/倒计时/收尾时返回 false 并写入可读原因。
    /// 会先预抓一帧确认屏幕可读（Wayland、macOS 未授权都会在这一步暴露），
    /// 同时用它的尺寸作为视频尺寸 —— 比按 DPR 推算更可靠。
    bool start(const Config &config, QString *error);

    /// 异步停止：立刻返回，收尾完成后发 finished()。面板按 Stopping 态渲染。
    void stop();

    /// 同步停止：仅供 aboutToQuit / 析构使用。会把排队中的帧写完再返回。
    void stopSync(int timeoutMs = 5000);

    /// 取消倒计时（还没开始抓帧，直接回 Idle）。
    void cancelCountdown();

    /// 默认输出目录（`视频/cxk-tools`），不存在时不保证创建。
    static QString defaultOutputDir();

signals:
    void stateChanged(screenrec::RecordService::State state);
    /// 每 500ms 一次的无参通知：面板收到后自己调 stats() 取快照。
    /// 不用带参数的信号是为了避免跨模块注册嵌套结构体的元类型。
    void statsChanged();
    void finished(const QString &path);
    void failed(const QString &message);
    void cancelled();

private:
    RecordService();

    void beginCountdown(const QRect &screenGeometry);
    void beginRecording();
    void tick();
    void onWriterFinished();
    /// 收尾：读统计、决定最终状态、发信号。threadStopped 为 false 表示写盘线程
    /// 没在预算内结束（极端情况），此时不能再碰队列与 worker。
    void collectResult(bool threadStopped);
    void setState(State state);
    void shutdownForQuit();

    QString nextOutputPath(const QString &baseDir) const;

    State m_state = State::Idle;
    Config m_config;
    Target m_target;

    AviMjpegWriter *m_writer = nullptr;
    FrameQueue *m_queue = nullptr;
    RecorderWorker *m_worker = nullptr;
    QThread *m_thread = nullptr;

    QTimer *m_tick = nullptr;
    QTimer *m_statsTimer = nullptr;
    QPointer<CountdownOverlay> m_countdownOverlay;

    QElapsedTimer m_clock;
    QImage m_firstFrame;      ///< 预抓的那一帧，录制开始时先写它，不浪费
    CursorBitmap m_firstCursor;
    QPoint m_firstCursorPos;
    bool m_firstCursorInside = false;

    qint64 m_scheduled = 0;   ///< 按帧率推算"此刻应该已经写了几帧"
    qint64 m_dropped = 0;
    qint64 m_skipped = 0;

    int m_width = 0;
    int m_height = 0;
    QString m_outputPath;
    QString m_lastOutput;
    QString m_lastError;
    bool m_collected = false;
    bool m_shuttingDown = false;
    /// 「允许开始抓帧」的准入票。不能拿 m_state 当门票：没开倒计时时状态还是
    /// Idle，而倒计时被取消后状态也是 Idle，两者无法区分 —— 前者必须开始录，
    /// 后者必须什么都不做。所以由真正决定开始的那一方（start / 倒计时走完）
    /// 发一张票，beginRecording() 验票入场。
    bool m_beginArmed = false;
};

} // namespace screenrec

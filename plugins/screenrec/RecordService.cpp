#include "RecordService.h"

#include "AviMjpegWriter.h"
#include "CountdownOverlay.h"
#include "CursorPainter.h"
#include "FrameQueue.h"
#include "RecorderWorker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QtDebug>

namespace screenrec {

namespace {

/// 一次最多补几帧。超过 4 帧说明系统已经卡到无法实时编码，再补只会让文件
/// 膨胀、队列永久落后；此时宁可把欠账清掉、让总时长短几百毫秒。
constexpr int kMaxCopies = 4;

/// 超过这个像素数就认为是"大屏"，队列容量要降。
/// 实测 2560×1600（410 万像素）整屏抓 28ms + 编码 24ms，15fps（66ms）够用；
/// 3840×2160（830 万）抓屏就要 45ms 以上，15fps 必然掉帧。
/// 阈值取 500 万，正好把 4K 圈进去、把 2.5K 留在外面。
constexpr qint64 kBigFramePixels = 5000000;

} // namespace

RecordService::RecordService()
{
    m_tick = new QTimer(this);
    // 默认的 CoarseTimer 允许 5% 误差，30fps 下每帧能偏 1.6ms，累积起来肉眼可见。
    m_tick->setTimerType(Qt::PreciseTimer);
    connect(m_tick, &QTimer::timeout, this, &RecordService::tick);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(500);
    connect(m_statsTimer, &QTimer::timeout, this, &RecordService::statsChanged);

    // 退出收尾。为什么必须在这里做：
    //   宿主所有退出路径都走 qApp->quit()（AppController），不会给窗口发 closeEvent，
    //   而且 MainWindow::prepareQuit() 会先销毁工具面板。等面板析构再收尾已经太晚。
    // 为什么不用 qAddPostRoutine：它登记的是**本 DLL 里的函数指针**，宿主
    // PluginManager::unloadAll() 调 FreeLibrary 之后那个指针就悬空了，而 post routine
    // 恰恰是在那之后才执行。
    // DirectConnection：aboutToQuit 时事件循环还在，同步收尾是安全的，也正是我们要的。
    if (QCoreApplication *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this,
                [this] { shutdownForQuit(); }, Qt::DirectConnection);
    }
}

RecordService::~RecordService()
{
    // 兜住"宿主直接卸载插件"这条路径（正常只在进程退出时发生）。
    // 此时不能再发信号、不能碰任何 QWidget，只做文件收尾。
    m_shuttingDown = true;
    stopSync(5000);
}

RecordService *RecordService::instance()
{
    // 函数内 static：构造发生在第一次 createPanel()，那时 QApplication 一定存在；
    // 析构发生在 DLL 卸载时。刻意不 mount 到 qApp 名下 —— DLL 先卸载的话，
    // qApp 会去 delete 一段已经 unmap 的 vtable。
    static RecordService service;
    return &service;
}

QString RecordService::defaultOutputDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty())
        dir = QDir::currentPath();
    return dir + QStringLiteral("/cxk-tools");
}

QString RecordService::nextOutputPath(const QString &baseDir) const
{
    const QString dir = baseDir.isEmpty() ? defaultOutputDir() : baseDir;
    QDir().mkpath(dir);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString path = dir + QStringLiteral("/screen-%1.avi").arg(stamp);
    // 同一秒内连开两次不能互相覆盖
    int suffix = 2;
    while (QFile::exists(path))
        path = dir + QStringLiteral("/screen-%1-%2.avi").arg(stamp).arg(suffix++);
    return path;
}

qint64 RecordService::elapsedMs() const
{
    if (m_state != State::Recording && m_state != State::Stopping)
        return 0;
    return m_clock.isValid() ? m_clock.elapsed() : 0;
}

RecordService::Stats RecordService::stats() const
{
    Stats snapshot;
    snapshot.width = m_width;
    snapshot.height = m_height;
    snapshot.path = m_outputPath;
    snapshot.dropped = m_dropped;
    snapshot.skipped = m_skipped;
    snapshot.elapsedMs = elapsedMs();

    if (m_worker) {
        const WorkerStats worker = m_worker->stats();
        snapshot.frames = worker.frames;
        snapshot.dups = worker.dups;
        snapshot.bytes = worker.bytes;
    }
    if (snapshot.elapsedMs > 0) {
        snapshot.actualFps = double(snapshot.frames) * 1000.0 / double(snapshot.elapsedMs);
    }
    return snapshot;
}

void RecordService::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

bool RecordService::start(const Config &config, QString *error)
{
    const auto fail = [&](const QString &message) {
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    };

    if (m_state == State::Recording || m_state == State::Countdown
        || m_state == State::Stopping) {
        return fail(QStringLiteral("正在录制中，请先停止"));
    }
    if (m_shuttingDown)
        return fail(QStringLiteral("程序正在退出"));

    // ---- 解析目标屏幕。存名字而不是指针：屏幕可能在录制前被拔掉。
    QScreen *screen = nullptr;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *candidate : screens) {
        if (candidate && candidate->name() == config.screenName) {
            screen = candidate;
            break;
        }
    }
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return fail(QStringLiteral("找不到可录制的屏幕"));

    m_config = config;
    m_config.fps = qBound(1, config.fps, 60);
    m_config.jpegQuality = qBound(10, config.jpegQuality, 100);
    m_target.screen = screen;
    m_target.regionLogic = (config.mode == Config::Mode::Region) ? config.regionLogic : QRect();

    // ---- 预抓一帧。这一步同时解决三件事：
    //   1) 立刻暴露"抓不到屏幕"（Wayland / macOS 未授权 / 远程桌面），
    //      而不是等用户录了三分钟才发现文件是黑的；
    //   2) 用实际抓到的尺寸做视频尺寸，比按 DPR 推算可靠；
    //   3) 这一帧不浪费，会作为第一帧写进视频。
    const GrabResult probe = grab(m_target, m_config.drawCursor);
    if (!probe.ok)
        return fail(probe.error);

    m_width = probe.image.width();
    m_height = probe.image.height();
    if (m_width < 2 || m_height < 2)
        return fail(QStringLiteral("录制区域过小"));

    m_firstFrame = probe.image;
    m_firstCursorInside = probe.cursorInside;
    m_firstCursorPos = probe.cursorInFrame;
    m_firstCursor = (m_config.drawCursor && probe.cursorInside) ? currentCursorBitmap()
                                                                : CursorBitmap();

    // ---- 输出路径
    m_outputPath = nextOutputPath(config.outputDir);
    const QFileInfo info(m_outputPath);
    if (!QDir().mkpath(info.absolutePath()))
        return fail(QStringLiteral("无法创建输出目录：%1").arg(info.absolutePath()));

    m_lastError.clear();
    m_collected = false;

    if (m_config.countdown)
        beginCountdown(screen->geometry());
    else {
        // 不开倒计时：没有等待窗口，直接放行。
        m_beginArmed = true;
        beginRecording();
    }
    return true;
}

void RecordService::beginCountdown(const QRect &screenGeometry)
{
    setState(State::Countdown);
    m_countdownOverlay = new CountdownOverlay(
        screenGeometry, 3,
        // 走完全部档位 → 真正开始抓帧
        [this] {
            m_beginArmed = true;
            beginRecording();
        },
        // 用户点击取消
        [this] {
            m_beginArmed = false;
            setState(State::Idle);
            emit cancelled();
        });
    m_countdownOverlay->start();
}

void RecordService::cancelCountdown()
{
    if (m_state != State::Countdown)
        return;
    // 先撤票：无论走哪条路，都不该再开始录了。
    m_beginArmed = false;

    CountdownOverlay *overlay = m_countdownOverlay;
    m_countdownOverlay = nullptr;
    if (overlay) {
        // cancel() 在"倒计时已走完、正在等浮层退场的 260ms 里"会直接 return
        // （m_finished 已置位），ops 里的 onCancelled 不会被调用。那 260ms 内
        // 用户点停止是很可能的（数字消失了，他以为已经在录），所以状态要在这里兜住。
        overlay->cancel();
        if (m_state == State::Countdown) {
            setState(State::Idle);
            emit cancelled();
        }
        return;
    }
    setState(State::Idle);
    emit cancelled();
}

void RecordService::beginRecording()
{
    // 没有准入票 = 这场录制没被允许开始（倒计时被取消、或者程序正在退出）。
    // 票是一次性的，进门就作废。
    if (m_shuttingDown || !m_beginArmed) {
        m_beginArmed = false;
        if (m_state == State::Countdown)
            setState(State::Idle);
        return;
    }
    m_beginArmed = false;

    m_writer = new AviMjpegWriter;
    QString error;
    if (!m_writer->open(m_outputPath, m_width, m_height, m_config.fps, &error)) {
        delete m_writer;
        m_writer = nullptr;
        setState(State::Error);
        m_lastError = error;
        emit failed(error);
        return;
    }

    // 4K 下每帧 33MB，容量 8 就是 260MB；降到 4 把上限压到 130MB。
    const bool bigFrame = qint64(m_width) * m_height > kBigFramePixels;
    m_queue = new FrameQueue(bigFrame ? 4 : FrameQueue::kDefaultCapacity);
    m_worker = new RecorderWorker(m_writer, m_queue, m_config.jpegQuality);

    // 预抓的那一帧先入队：保证第一帧就是用户按下开始那一刻的画面
    FrameJob first;
    first.frame = m_firstFrame;
    first.drawCursor = m_config.drawCursor;
    first.cursorInside = m_firstCursorInside;
    first.cursorPos = m_firstCursorPos;
    first.cursor = m_firstCursor;
    first.copies = 1;
    m_queue->push(std::move(first));
    m_scheduled = 1;
    m_firstFrame = QImage();   // 交出去之后就别再持有一份 16MB 的图

    m_thread = QThread::create([this] { m_worker->run(); });
    connect(m_thread, &QThread::finished, this, &RecordService::onWriterFinished);
    m_thread->start();

    m_dropped = 0;
    m_skipped = 0;
    m_clock.start();

    m_tick->setInterval(qMax(5, 1000 / m_config.fps));
    m_tick->start();
    m_statsTimer->start();

    setState(State::Recording);
}

void RecordService::tick()
{
    if (m_state != State::Recording || !m_queue)
        return;

    // ---- 目标帧数：按经过的时间算"此刻总共应该写了几帧"，而不是"每次 tick 写一帧"。
    // 定时器会迟到（抓屏自己就要 28ms），按时间算才能让帧率和真实时间对齐。
    const qint64 want = m_clock.elapsed() * m_config.fps / 1000;
    const qint64 need = want - m_scheduled;
    if (need <= 0)
        return;   // 这一拍还不该出帧，省一次 BitBlt

    int copies = int(qMin<qint64>(need, kMaxCopies));
    if (need > kMaxCopies) {
        // 落后太多：多出来的直接跳过，并把账清算，
        // 否则欠账会越滚越大、队列永远追不上。
        m_skipped += need - kMaxCopies;
        m_scheduled = want;
    }

    const GrabResult grabbed = grab(m_target, m_config.drawCursor);
    if (grabbed.busy) {
        // 上一帧还在抓（grabWindow 在 Windows 上会转消息泵，定时器可能重入）。
        // 这是高频正常现象，只计数不刷日志。
        ++m_skipped;
        return;
    }
    if (!grabbed.ok) {
        // 屏幕被拔掉 / 会话锁定 / 受保护内容：停止并保存已录的部分，
        // 让用户拿到一个能播的文件，而不是一个永远停在那一帧的假录制。
        const QString message = grabbed.error;
        stop();
        m_lastError = message;
        emit failed(message);
        return;
    }

    FrameJob job;
    job.frame = grabbed.image;
    job.drawCursor = m_config.drawCursor;
    job.cursorInside = grabbed.cursorInside;
    job.cursorPos = grabbed.cursorInFrame;
    if (job.drawCursor && job.cursorInside)
        job.cursor = currentCursorBitmap();
    job.copies = copies;

    if (!m_queue->push(std::move(job))) {
        // 队列满（编码跟不上）：丢**新**帧而不是旧帧 —— 丢旧帧会让画面时间倒退；
        // 同时把账清算，避免 need 越积越大。
        ++m_dropped;
        m_scheduled = want;
        return;
    }
    m_scheduled += copies;
}

void RecordService::stop()
{
    if (m_state == State::Countdown) {
        cancelCountdown();
        return;
    }
    if (m_state != State::Recording)
        return;

    setState(State::Stopping);
    m_tick->stop();
    m_statsTimer->stop();
    // 关队列让写盘线程把剩下的帧消费完、写完索引，然后它自己结束，
    // finished 信号会回到 onWriterFinished()。
    if (m_queue)
        m_queue->close();
}

void RecordService::stopSync(int timeoutMs)
{
    if (m_state == State::Countdown) {
        cancelCountdown();
        return;
    }
    if (m_state != State::Recording && m_state != State::Stopping)
        return;

    m_tick->stop();
    m_statsTimer->stop();
    if (m_queue)
        m_queue->close();

    QThread *thread = m_thread;
    m_thread = nullptr;   // 先置空：finished 信号即使随后到达也不会重复处理
    bool stopped = true;
    if (thread) {
        stopped = thread->wait(timeoutMs);
        if (stopped) {
            delete thread;
        } else {
            // 写盘线程卡在磁盘 IO 上。这里不能 delete（会 terminate 一个正在写
            // 文件头的线程，留下半截 header），只能放弃它 —— 文件靠 checkpoint 保活。
            qWarning() << "screenrec: 写盘线程未在预算内结束，已保存的部分仍可顺序播放";
        }
    }
    collectResult(stopped);
}

void RecordService::onWriterFinished()
{
    QThread *thread = m_thread;
    m_thread = nullptr;
    if (thread)
        thread->deleteLater();
    collectResult(true);
}

void RecordService::collectResult(bool threadStopped)
{
    if (m_collected)
        return;
    m_collected = true;

    m_tick->stop();
    m_statsTimer->stop();

    WorkerStats worker;
    QString writerError;
    if (m_worker)
        worker = m_worker->stats();
    if (m_writer)
        writerError = m_writer->errorString();

    // 队列与 worker 只有在写盘线程确实结束之后才能删。
    // 超时的那条路径上线程可能还在用它们，宁可泄漏到进程退出。
    if (threadStopped) {
        delete m_queue;
        m_queue = nullptr;
        delete m_worker;
        m_worker = nullptr;
        delete m_writer;
        m_writer = nullptr;
    } else {
        m_queue = nullptr;
        m_worker = nullptr;
        m_writer = nullptr;
    }

    const QString path = m_outputPath;

    if (worker.failed || !writerError.isEmpty()) {
        const QString message = worker.failed ? worker.error : writerError;
        m_lastError = message;
        setState(State::Error);
        emit failed(message);
    } else if (worker.frames <= 0) {
        // 一帧都没写成：留着 224 字节的空头只会让用户困惑，删掉并明确说原因。
        QFile::remove(path);
        m_lastError = QStringLiteral("没有录制到任何画面");
        setState(State::Error);
        emit failed(m_lastError);
    } else {
        m_lastOutput = path;
        setState(State::Idle);
        emit finished(path);
    }

    m_outputPath.clear();
    m_clock.invalidate();
    m_scheduled = 0;
}

void RecordService::shutdownForQuit()
{
    m_shuttingDown = true;
    stopSync(5000);
}

} // namespace screenrec

#include "ScrollSession.h"

#include "ScreenGrab.h"
#include "ThemePalette.h"

#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

ScrollSession::ScrollSession(const QRect &region, qreal dpr, int maxHeight)
    : QWidget(nullptr),
      m_region(region),
      m_dpr(dpr > 0.0 ? dpr : 1.0),
      m_maxHeight(maxHeight > 0 ? maxHeight : 20000)
{
    // 不抢焦点是这里的关键：用户正在目标窗口里滚鼠标，控制条一出现就把焦点
    // 抢过来会让滚轮落到控制条上（Windows 默认「悬停滚动非活动窗口」关掉时），
    // 表现为「一弹出就滚不动了」。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                   | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_StyledBackground, true);   // 无边框窗口的底色要靠自绘/样式表

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("滚动拼接中"), this);
    xtools::theme::applyStyle(title, QStringLiteral(
        "QLabel{color:@text;font-size:13px;font-weight:600;background:transparent;}"));
    row->addWidget(title);

    m_counter = new QLabel(this);
    xtools::theme::applyStyle(m_counter, QStringLiteral(
        "QLabel{color:@text-muted;font-size:12px;background:transparent;}"));
    row->addWidget(m_counter, 1);

    auto *doneBtn = new QPushButton(QStringLiteral("完成"), this);
    doneBtn->setProperty("accent", true);   // 全局 qss 的强调按钮样式
    auto *cancelBtn = new QPushButton(QStringLiteral("取消"), this);
    row->addWidget(doneBtn);
    row->addWidget(cancelBtn);
    layout->addLayout(row);

    auto *hint = new QLabel(this);
    hint->setWordWrap(true);
    hint->setText(QStringLiteral(
        "用鼠标滚轮滚动目标窗口的内容（上下都行），新出现的部分会自动接到长图上；"
        "停止滚动约 2.5 秒后自动完成。若目标窗口没反应，先点它一下让它获得焦点。"));
    xtools::theme::applyStyle(hint, QStringLiteral(
        "QLabel{color:@text-faint;font-size:11px;background:transparent;}"));
    layout->addWidget(hint);

    xtools::theme::applyStyle(this, QStringLiteral(
        "ScrollSession{background:@bg;border:1px solid @border-strong;border-radius:10px;}"));

    setFixedWidth(380);
    adjustSize();
    placeBar();

    connect(doneBtn, &QPushButton::clicked, this, &ScrollSession::finishNow);
    connect(cancelBtn, &QPushButton::clicked, this, &ScrollSession::cancelNow);

    m_counter->setText(QStringLiteral("等待首次抓帧…"));

    m_timer = new QTimer(this);
    m_timer->setInterval(kIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &ScrollSession::tick);
    m_timer->start();
}

void ScrollSession::placeBar()
{
    QScreen *screen = QGuiApplication::screenAt(m_region.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);

    const QSize hint = sizeHint();
    const int w = qMax(380, hint.width());
    const int h = qMax(96, hint.height());

    // 优先放在选区下方 / 上方，这样控制条不会被自己截进画面。
    int y = m_region.bottom() + 12;
    if (y + h > area.bottom())
        y = m_region.top() - h - 12;
    // 选区占满整屏时无处可躲：压在屏幕底部。
    // 压在选区上不影响拼接：控制条是静止的，而位移检测只接受 dy≠0 的匹配，
    // 静止画面在任何非零位移下都对不上，最多是把这几个采样点算成「不匹配」。
    if (y < area.top())
        y = area.bottom() - h - 12;

    int x = m_region.center().x() - w / 2;
    x = qBound(area.left() + 8, x, area.right() - w - 8);
    y = qBound(area.top() + 8, y, area.bottom() - h - 8);
    setGeometry(x, y, w, h);
}

void ScrollSession::tick()
{
    if (m_finished)
        return;

    QImage frame = screengrab::grabRegion(m_region, nullptr);
    if (frame.isNull()) {
        if (++m_grabFailures >= kMaxGrabFailures) {
            abort(QStringLiteral("连续抓屏失败，已停止拼接。") + screengrab::platformHint());
        }
        return;
    }
    m_grabFailures = 0;

    // 统一成 ARGB32：逐像素比较走 QRgb 指针，格式不定会让结果依赖平台的默认格式
    if (frame.format() != QImage::Format_ARGB32)
        frame = frame.convertToFormat(QImage::Format_ARGB32);

    if (!m_canvas.isStarted()) {
        m_canvas.begin(frame, m_maxHeight);
        m_last = frame;
        updateCounter();
        return;
    }

    // 尺寸抖动（用户中途改了分辨率 / 跨屏拖动）时跳过这一帧，等尺寸稳定再说。
    // 不重开画布：已经拼好的部分不该因为一次抖动就丢掉。
    if (frame.size() != m_last.size())
        return;

    const stitch::Shift shift = stitch::findVerticalShift(m_last, frame, m_last.height() * 4 / 5);
    if (shift.usable()) {
        m_last = frame;   // 基准帧永远是最新一帧，往上/往下滚都能接上
        if (m_canvas.addFrame(frame, shift.dy))
            ++m_mergeCount;
        m_stableTicks = 0;
        updateCounter();
    } else {
        ++m_stableTicks;
        // 已经拼出内容，且内容稳定了一段时间 → 认为滚完了，自动收尾
        if (m_mergeCount > 0 && m_stableTicks >= kStableTicks) {
            finishNow();
            return;
        }
        // 一帧都没拼上（用户可能在无可滚动内容的窗口上）→ 超时后按区域截图收尾
        if (m_mergeCount == 0 && m_stableTicks >= kIdleTicks) {
            finishNow();
            return;
        }
    }

    if (m_canvas.limitReached())
        finishNow();
}

void ScrollSession::updateCounter()
{
    if (!m_counter)
        return;
    m_counter->setText(QStringLiteral("已拼 %1 帧 · 高 %2 像素")
                           .arg(m_canvas.frameCount())
                           .arg(m_canvas.height()));
}

void ScrollSession::finishNow()
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_timer)
        m_timer->stop();

    QImage image = m_canvas.image();
    if (!image.isNull())
        image.setDevicePixelRatio(m_dpr);

    hide();
    emit finished(image, m_canvas.limitReached());
    deleteLater();   // 槽函数里可能已经把面板连同本对象一起收掉，不能直接 delete this
}

void ScrollSession::cancelNow()
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_timer)
        m_timer->stop();
    hide();
    emit cancelled();
    deleteLater();
}

void ScrollSession::abort(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    if (m_timer)
        m_timer->stop();
    hide();
    emit failed(reason);
    deleteLater();
}

void ScrollSession::closeEvent(QCloseEvent *event)
{
    // 没有标题栏，正常点不到关闭按钮；但系统（Alt+F4、任务切换器）仍可能关掉它，
    // 这一路必须走取消，否则宿主窗口会一直停在隐藏状态。
    if (!m_finished)
        cancelNow();
    event->accept();
}

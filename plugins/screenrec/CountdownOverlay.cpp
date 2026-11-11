#include "CountdownOverlay.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

namespace screenrec {

CountdownOverlay::CountdownOverlay(const QRect &screenGeometry, int seconds,
                                   std::function<void()> onFinished,
                                   std::function<void()> onCancelled)
    : QWidget(nullptr),
      m_total(qMax(1, seconds)),
      m_onFinished(std::move(onFinished)),
      m_onCancelled(std::move(onCancelled))
{
    // Frameless + 置顶 + 不接受焦点：
    // WindowDoesNotAcceptFocus 是不抢焦点的关键 —— 否则被录窗口的标题栏会变灰，
    // 而且焦点一旦被夺走，用户按住的方向键之类也会断掉。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                   | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    // **必须**是半透明窗口：paintEvent 里只画一层 alpha=110 的压暗，
    // 想让这层压暗叠在真实桌面上，窗口本身就不能有不透明底色。
    // 这里一开始写的是 false，结果整个屏幕被窗口的调色板灰底盖死 ——
    // 倒计时那 2.4 秒里用户完全看不见自己要录的画面，与"轻压暗、留着确认"
    // 的设计意图正相反。
    // 代价：不能再用静态截图当底（那会把画面冻住，而用户正是要在这几秒里摆窗口）。
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setGeometry(screenGeometry);

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(kStepMs);
    connect(m_timer, &QTimer::timeout, this, &CountdownOverlay::advance);
}

void CountdownOverlay::start()
{
    m_remaining = m_total;
    show();
    raise();
    update();
    m_timer->start();
}

void CountdownOverlay::advance()
{
    if (m_finished)
        return;

    --m_remaining;
    if (m_remaining > 0) {
        update();
        return;
    }

    m_finished = true;
    m_timer->stop();
    hide();

    // 先让合成层彻底撤掉浮层，再开始抓帧，否则第一帧里会有一个大大的"1"。
    // 绑定 this 作 context：对象若被提前销毁，这个回调不会执行。
    QTimer::singleShot(kSettleMs, this, [this] {
        if (m_onFinished)
            m_onFinished();
        deleteLater();
    });
}

void CountdownOverlay::cancel()
{
    if (m_finished)
        return;
    m_finished = true;
    m_timer->stop();
    hide();
    if (m_onCancelled)
        m_onCancelled();
    deleteLater();
}

void CountdownOverlay::mousePressEvent(QMouseEvent *event)
{
    // 不抢焦点就拿不到 Esc，所以取消方式只能是点击。顶部提示里写清了这一点。
    if (event->button() == Qt::LeftButton) {
        cancel();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CountdownOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // 轻微压暗（不是全黑）：用户还要在这一刻确认"要录的画面在不在视野里"。
    painter.fillRect(rect(), QColor(10, 14, 22, 110));

    // 大数字
    QFont numberFont = font();
    numberFont.setPixelSize(qBound(64, height() / 6, 160));
    numberFont.setBold(true);
    painter.setFont(numberFont);
    painter.setPen(QColor(255, 255, 255));
    painter.drawText(rect(), Qt::AlignCenter, QString::number(qMax(1, m_remaining)));

    // 底部提示：说明怎么取消，以及倒计时结束就会开始录
    QFont tipFont = font();
    tipFont.setPixelSize(14);
    painter.setFont(tipFont);
    const QRect tipRect(0, height() - 120, width(), 40);
    painter.setPen(QColor(255, 255, 255, 220));
    painter.drawText(tipRect, Qt::AlignCenter,
                     QStringLiteral("%1 秒后开始录制 · 点击任意位置取消")
                         .arg(qMax(1, m_remaining)));

    // 中间上方补一句范围说明：用户此刻最想确认的是"录的是哪块"
    QFont captionFont = font();
    captionFont.setPixelSize(13);
    painter.setFont(captionFont);
    painter.setPen(QColor(255, 255, 255, 150));
    painter.drawText(QRect(0, 48, width(), 30), Qt::AlignCenter,
                     QStringLiteral("录制即将开始，请把要录的窗口摆好"));
}

} // namespace screenrec

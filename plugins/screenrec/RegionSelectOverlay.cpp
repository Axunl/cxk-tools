#include "RegionSelectOverlay.h"

#include <QCloseEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>

namespace screenrec {

RegionSelectOverlay::RegionSelectOverlay(const QRect &screenGeometry, const QPixmap &screenShot,
                                         qreal dpr,
                                         std::function<void(const QRect &)> onSelected,
                                         std::function<void()> onCancelled)
    : QWidget(nullptr),
      m_shot(screenShot),
      m_dpr(dpr > 0.0 ? dpr : 1.0),
      m_onSelected(std::move(onSelected)),
      m_onCancelled(std::move(onCancelled))
{
    // 与 OCR 浮层一致：Frameless + 置顶 + 用 Qt::Window 拿键盘焦点（Esc 取消依赖它）。
    // 这里跟倒计时浮层不同 —— 框选是"用户主动操作"的模态交互，抢焦点是对的。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_StyledBackground, false);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setGeometry(screenGeometry);
}

QRect RegionSelectOverlay::normalizedSelection() const
{
    // 不要写成 QRect(m_start, m_current).normalized()：QRect 存的是闭区间端点，
    // 这个构造函数会把 "右下角" 原样当端点存进去，再 normalized() 时为了保持
    // "端点仍在像素中心" 的语义，会在反转的两边各内缩 1 像素。
    // 实测：从 (400,300) 拖到 (900,700) 得 501×401，反过来拖则得 499×399 ——
    // 同一块区域，用户看到两个不同的视频尺寸。用 min/max 显式构造，两个方向
    // 都稳定给出 |dx|+1，语义也更好读。
    const int left = qMin(m_start.x(), m_current.x());
    const int top = qMin(m_start.y(), m_current.y());
    const int right = qMax(m_start.x(), m_current.x());
    const int bottom = qMax(m_start.y(), m_current.y());
    const QRect raw(left, top, right - left + 1, bottom - top + 1);
    return raw.intersected(rect());
}

void RegionSelectOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    // 底图按 devicePixelRatio 自动映射到逻辑坐标，不需要手动缩放
    // （手动缩到逻辑尺寸再放大回来等于两次插值，非整数缩放下会明显发糊）。
    painter.drawPixmap(0, 0, m_shot);

    // 整屏压暗，让"现在在框选"这件事一眼可见；选区内用原图覆盖回来。
    painter.fillRect(rect(), QColor(15, 23, 42, 96));

    const QRect selection = normalizedSelection();
    if (m_dragging && !selection.isEmpty()) {
        const QRect src(qRound(selection.x() * m_dpr),
                        qRound(selection.y() * m_dpr),
                        qRound(selection.width() * m_dpr),
                        qRound(selection.height() * m_dpr));
        painter.drawPixmap(selection, m_shot, src);
        painter.setPen(QPen(QColor(59, 130, 246), 2));
        painter.drawRect(selection.adjusted(0, 0, -1, -1));

        // 尺寸提示用**物理像素**：录出来的视频就是这么宽高，显示逻辑像素会让
        // 用户在 150% 缩放的屏幕上看到一个对不上的数字。
        const QString tip = QStringLiteral("%1 × %2")
                                .arg(qRound(selection.width() * m_dpr))
                                .arg(qRound(selection.height() * m_dpr));
        QFontMetrics metrics(painter.font());
        const int textWidth = metrics.horizontalAdvance(tip);
        const QRect badge(selection.left(),
                          qMax(4, selection.top() - 28),
                          textWidth + 18, 24);
        painter.fillRect(badge, QColor(15, 23, 42, 220));
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, tip);
    }

    // 顶部提示：浮层没有标题栏，不写这句用户不知道 Esc 能取消、也不知道会录成多大。
    const QString tip = QStringLiteral("拖动鼠标框选要录制的区域 · Esc 取消");
    QFont tipFont = painter.font();
    tipFont.setPointSize(11);
    painter.setFont(tipFont);
    const QFontMetrics tipMetrics(tipFont);
    const int boxWidth = tipMetrics.horizontalAdvance(tip) + 36;
    const QRect tipBox((width() - boxWidth) / 2, 24, boxWidth, 38);
    painter.fillRect(tipBox, QColor(15, 23, 42, 210));
    painter.setPen(Qt::white);
    painter.drawText(tipBox, Qt::AlignCenter, tip);
}

void RegionSelectOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_finished) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_start = event->pos();
    m_current = m_start;
    m_dragging = true;
    update();
}

void RegionSelectOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    // 夹在窗口内，避免拖出屏幕后归一化出的矩形比屏幕还大
    m_current.setX(qBound(0, event->pos().x(), width() - 1));
    m_current.setY(qBound(0, event->pos().y(), height() - 1));
    update();
}

void RegionSelectOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    finishSelection();
}

void RegionSelectOverlay::keyPressEvent(QKeyEvent *event)
{
    // 只认裸 Esc：浮层是临时模态交互，不该让主窗口的 Esc 分层逻辑介入
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        event->accept();
        cancel();
        return;
    }
    QWidget::keyPressEvent(event);
}

void RegionSelectOverlay::closeEvent(QCloseEvent *event)
{
    // Alt+F4 / 系统关闭也要走取消回调，否则调用方会一直等一个永远不来的结果
    if (!m_finished)
        cancel();
    event->accept();
}

void RegionSelectOverlay::finishSelection()
{
    const QRect selection = normalizedSelection();
    if (selection.width() < kMinSelectionPx || selection.height() < kMinSelectionPx) {
        cancel();   // 单击 / 极细的拖拽视为误操作
        return;
    }
    if (m_finished)
        return;
    m_finished = true;

    hide();
    if (m_onSelected)
        m_onSelected(selection);
    deleteLater();   // 回调可能已经改了调用方状态，不能在栈上直接 delete this
}

void RegionSelectOverlay::cancel()
{
    if (m_finished)
        return;
    m_finished = true;
    hide();
    if (m_onCancelled)
        m_onCancelled();
    deleteLater();
}

} // namespace screenrec

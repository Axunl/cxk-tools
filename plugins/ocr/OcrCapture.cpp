#include "OcrCapture.h"

#include <QCloseEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QtGlobal>

OcrCaptureOverlay::OcrCaptureOverlay(const QRect &screenGeometry,
                                     const QPixmap &screenShot,
                                     qreal dpr,
                                     std::function<void(const QPixmap &)> onCaptured,
                                     std::function<void()> onCancelled)
    : QWidget(nullptr),
      m_shot(screenShot),
      m_dpr(dpr > 0.0 ? dpr : 1.0),
      m_onCaptured(std::move(onCaptured)),
      m_onCancelled(std::move(onCancelled))
{
    // Frameless + 置顶：截图期间必须盖住桌面上所有窗口。
    // 用 Qt::Window 而不是 Qt::Tool，保证能拿到键盘焦点（Esc 取消依赖它）。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_StyledBackground, false);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setGeometry(screenGeometry);

    // 显示用的底图与裁剪用的原图是同一份物理像素（m_shot 本身就是 grabWindow
    // 抓到的设备像素图，自带正确的 devicePixelRatio）。这里不额外 scaled 缩小：
    // 之前把物理像素平滑缩到逻辑尺寸再让 Qt 按 DPR 放大回来，等于两次插值，
    // 在 125%/150% 这类非整数缩放下会把底图弄得明显发糊。Qt 会按 m_shot 的
    // devicePixelRatio 在绘制时自动映射到逻辑坐标，无需也不应手动缩放。
    m_display = m_shot;
}

QRect OcrCaptureOverlay::normalizedSelection() const
{
    const QRect raw = QRect(m_start, m_current).normalized();
    return raw.intersected(rect());
}

void OcrCaptureOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.drawPixmap(0, 0, m_display);

    // 整屏压暗：让用户一眼看出「现在在截图模式」。选区内用原图覆盖回来。
    painter.fillRect(rect(), QColor(15, 23, 42, 96));

    const QRect selection = normalizedSelection();
    if (m_dragging && !selection.isEmpty()) {
        // 源矩形是逻辑坐标，m_display 是物理像素图，必须乘 DPR 才能取对像素。
        // 用三参数 drawPixmap(target, pixmap, source) 时 Qt 会按 pixmap 的 DPR
        // 自动映射 source，但这里显式换算更直白、不依赖版本差异。
        const QRect src(qRound(selection.x() * m_dpr),
                        qRound(selection.y() * m_dpr),
                        qRound(selection.width() * m_dpr),
                        qRound(selection.height() * m_dpr));
        painter.drawPixmap(selection, m_display, src);
        painter.setPen(QPen(QColor(59, 130, 246), 2));
        painter.drawRect(selection.adjusted(0, 0, -1, -1));

        // 尺寸提示跟着选区走，方便判断框得够不够大（太小的图 OCR 识别率会掉）
        const QString sizeTip = QStringLiteral("%1 × %2")
                                    .arg(selection.width()).arg(selection.height());
        QFontMetrics metrics(painter.font());
        const int textWidth = metrics.horizontalAdvance(sizeTip);
        const QRect badge(selection.right() - textWidth - 20,
                          qMin(selection.bottom() + 6, height() - 26),
                          textWidth + 16, 22);
        painter.fillRect(badge, QColor(15, 23, 42, 210));
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, sizeTip);
    }

    // 顶部操作提示：截图浮层没有标题栏，不写这句用户不知道 Esc 能取消
    const QString tip = QStringLiteral("拖动鼠标框选要识别的区域 · Esc 取消");
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

void OcrCaptureOverlay::mousePressEvent(QMouseEvent *event)
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

void OcrCaptureOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    // 夹在窗口内，避免拖出屏幕后归一化矩形比屏幕还大
    m_current.setX(qBound(0, event->pos().x(), width() - 1));
    m_current.setY(qBound(0, event->pos().y(), height() - 1));
    update();
}

void OcrCaptureOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    finishSelection();
}

void OcrCaptureOverlay::keyPressEvent(QKeyEvent *event)
{
    // 只认裸 Esc：截图浮层是临时模态交互，不该让主窗口的 Esc 分层逻辑介入
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        event->accept();
        cancel();
        return;
    }
    QWidget::keyPressEvent(event);
}

void OcrCaptureOverlay::closeEvent(QCloseEvent *event)
{
    // Alt+F4 / 系统关闭也要走取消回调，否则宿主窗口会一直停在隐藏状态
    if (!m_finished)
        cancel();
    event->accept();
}

void OcrCaptureOverlay::finishSelection()
{
    const QRect selection = normalizedSelection();
    if (selection.width() < kMinSelectionPx || selection.height() < kMinSelectionPx) {
        cancel();   // 单击/极细的拖拽视为误操作
        return;
    }
    if (m_finished)
        return;
    m_finished = true;

    // 逻辑像素 → 设备像素。QPixmap::rect()/copy() 用的是设备像素，
    // 不乘 DPR 会在高 DPI 屏上截到左上角四分之一区域。
    const QRect deviceRect(qRound(selection.x() * m_dpr),
                           qRound(selection.y() * m_dpr),
                           qRound(selection.width() * m_dpr),
                           qRound(selection.height() * m_dpr));
    const QRect clipped = deviceRect.intersected(m_shot.rect());
    QPixmap cropped = m_shot.copy(clipped);
    cropped.setDevicePixelRatio(m_dpr);

    hide();
    if (m_onCaptured)
        m_onCaptured(cropped);
    deleteLater();   // 回调可能已把面板销毁，不能在栈上直接 delete this
}

void OcrCaptureOverlay::cancel()
{
    if (m_finished)
        return;
    m_finished = true;
    hide();
    if (m_onCancelled)
        m_onCancelled();
    deleteLater();
}

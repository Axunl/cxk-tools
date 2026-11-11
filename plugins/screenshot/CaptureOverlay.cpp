#include "CaptureOverlay.h"

#include "ThemePalette.h"   // 强调色/提示文字色走主题令牌，避免深色主题下留白块

#include <QCloseEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QtGlobal>

namespace {

/// 选区外压暗用的半透明色。**刻意写死，不走主题令牌**：
/// 遮罩压的是用户桌面上任意内容（可能是白底网页，也可能是黑色终端），
/// 主题里的 overlay 令牌是给应用内浮层用的、透明度按 UI 调的，压在桌面
/// 截图上要么看不清选区、要么把桌面糊死。固定深蓝 + 中低透明度是最稳的。
const QColor kDimColor(15, 23, 42, 112);

/// 尺寸提示胶囊底色。同上，压在任何桌面内容上都必须可读，故写死深色。
const QColor kBadgeColor(15, 23, 42, 218);

} // namespace

CaptureOverlay::CaptureOverlay(const screengrab::Snapshot &snapshot,
                               const QString &tip,
                               std::function<void(const QRect &)> onSelected,
                               std::function<void()> onCancelled)
    : QWidget(nullptr),
      m_base(snapshot.compose()),
      m_virtualGeometry(snapshot.virtualGeometry),
      m_snapshot(snapshot),
      m_tip(tip),
      m_onSelected(std::move(onSelected)),
      m_onCancelled(std::move(onCancelled))
{
    // Frameless + 置顶：截图期间必须盖住桌面上所有窗口。
    // 用 Qt::Window 而不是 Qt::Tool，保证能拿到键盘焦点（Esc 取消依赖它）。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_StyledBackground, false);
    setAttribute(Qt::WA_DeleteOnClose, false);   // 生命周期由回调里的 deleteLater 管
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // 窗口几何 = 整块虚拟桌面（多屏时从左负坐标那块开始），
    // 于是「局部坐标 + virtualGeometry.topLeft()」就是全局坐标。
    setGeometry(m_virtualGeometry);
}

QPoint CaptureOverlay::toGlobal(const QPoint &local) const
{
    return local + m_virtualGeometry.topLeft();
}

QPoint CaptureOverlay::toLocal(const QPoint &global) const
{
    return global - m_virtualGeometry.topLeft();
}

QRect CaptureOverlay::selectionLocal() const
{
    return clampToScreen(QRect(m_start, m_current).normalized()).intersected(rect());
}

QRect CaptureOverlay::clampToScreen(const QRect &localRect) const
{
    // 以**拖拽起点**判定归属屏幕：用户可能从右屏拖向左屏，以终点判定会让
    // 选区在拖动过程中突然跳变，体验反而更怪。
    const int index = m_snapshot.screenIndexAt(toGlobal(m_start));
    if (index < 0)
        return localRect;
    const QRect screenLocal =
        m_snapshot.screens.at(index).logicalGeometry.translated(-m_virtualGeometry.topLeft());
    return localRect.intersected(screenLocal);
}

void CaptureOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.drawImage(0, 0, m_base);

    // 整屏压暗：让用户一眼看出「现在在截图」。选区内用底图覆盖回来。
    painter.fillRect(rect(), kDimColor);

    const QRect selection = selectionLocal();
    if (m_dragging && !selection.isEmpty()) {
        // 底图是逻辑尺寸（dpr=1），所以源矩形与目标矩形同坐标，不需要换算 DPR
        painter.drawImage(selection, m_base, selection);

        const QColor accent = xtools::theme::color(xtools::theme::Color::Accent);
        painter.setPen(QPen(accent, 2));
        painter.drawRect(selection.adjusted(0, 0, -1, -1));

        // 尺寸提示跟着选区走：滚动截图要用户连整块可滚动区域一起框进来，
        // 实时给出宽高比「目测差不多」靠谱得多。
        const QString sizeTip = QStringLiteral("%1 × %2").arg(selection.width()).arg(selection.height());
        QFont badgeFont = painter.font();
        badgeFont.setPointSize(10);
        painter.setFont(badgeFont);
        const QFontMetrics metrics(badgeFont);
        const int textWidth = metrics.horizontalAdvance(sizeTip);
        // 默认放在选区右下角外侧；贴到窗口边缘时改放内侧，避免被裁掉
        int badgeX = selection.right() - textWidth - 20;
        int badgeY = selection.bottom() + 8;
        if (badgeY + 24 > height())
            badgeY = selection.bottom() - 30;
        badgeX = qBound(4, badgeX, width() - textWidth - 24);
        badgeY = qBound(4, badgeY, height() - 28);
        const QRect badge(badgeX, badgeY, textWidth + 16, 24);
        painter.fillRect(badge, kBadgeColor);
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, sizeTip);
    }

    // 顶部操作提示：遮罩没有标题栏，不写这句用户不知道 Esc 能取消、
    // 也不知道滚动截图该怎么滚。
    if (!m_tip.isEmpty()) {
        QFont tipFont = painter.font();
        tipFont.setPointSize(11);
        painter.setFont(tipFont);
        const QFontMetrics tipMetrics(tipFont);
        const int boxWidth = qMin(width() - 40, tipMetrics.horizontalAdvance(m_tip) + 40);
        const QRect tipBox((width() - boxWidth) / 2, 24, boxWidth, 40);
        painter.fillRect(tipBox, kBadgeColor);
        painter.setPen(Qt::white);
        painter.drawText(tipBox, Qt::AlignCenter, m_tip);
    }
}

void CaptureOverlay::setCurrentPoint(const QPoint &local)
{
    const QPoint clamped(qBound(0, local.x(), width() - 1), qBound(0, local.y(), height() - 1));
    if (clamped == m_current)
        return;   // 鼠标在原地抖动时不重绘：全屏重绘一次是几十毫秒
    m_current = clamped;
    update();
}

void CaptureOverlay::mousePressEvent(QMouseEvent *event)
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

void CaptureOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    setCurrentPoint(event->pos());
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    m_current = event->pos();
    finishSelection();
}

void CaptureOverlay::keyPressEvent(QKeyEvent *event)
{
    // 只认裸 Esc：遮罩是临时模态交互，不该让主窗口的 Esc 分层逻辑介入
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        event->accept();
        cancel();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CaptureOverlay::closeEvent(QCloseEvent *event)
{
    // Alt+F4 / 系统关闭也要走取消回调，否则宿主窗口会一直停在隐藏状态
    if (!m_finished)
        cancel();
    event->accept();
}

void CaptureOverlay::finishSelection()
{
    const QRect selection = selectionLocal();
    if (selection.width() < kMinSelectionPx || selection.height() < kMinSelectionPx) {
        cancel();   // 单击 / 极细的拖拽视为误操作
        return;
    }
    if (m_finished)
        return;
    m_finished = true;

    const QRect global = selection.translated(m_virtualGeometry.topLeft());
    hide();
    if (m_onSelected)
        m_onSelected(global);
    deleteLater();
}

void CaptureOverlay::cancel()
{
    if (m_finished)
        return;
    m_finished = true;
    hide();
    if (m_onCancelled)
        m_onCancelled();
    deleteLater();
}

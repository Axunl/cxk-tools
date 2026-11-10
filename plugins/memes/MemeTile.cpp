#include "MemeTile.h"

#include "ThemePalette.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>

namespace meme {

namespace {

/// 角标高度。GIF 与「预览」两个角标共用，改一处就够。
constexpr int kBadgeHeight = 16;
constexpr int kBadgePadding = 5;

} // namespace

MemeTile::MemeTile(QWidget *parent)
    : QWidget(parent)
{
    // objectName 供 --shot 与界面回归里认控件用（瓦片没有 Q_OBJECT，不能靠类型名找）。
    setObjectName(QStringLiteral("memeTile"));
    setFixedSize(kTileWidth, kTileHeight);
    // 让自绘卡片之外的地方不抢鼠标；父级滚动区要靠滚轮滚动。
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
}

void MemeTile::setItem(const MemeItem &item, int index)
{
    m_item = item;
    m_index = index;

    const QString tooltip = item.title.isEmpty() ? item.thumbUrl : item.title;
    setToolTip(tooltip);
    update();
}

void MemeTile::setPoster(const QPixmap &poster, bool animated)
{
    m_poster = poster;
    m_animated = animated;
    m_failure.clear();
    update();
}

void MemeTile::setFailure(const QString &reason)
{
    stopMovie();
    m_poster = QPixmap();
    m_failure = reason;
    update();
}

void MemeTile::clearImage()
{
    stopMovie();
    m_poster = QPixmap();
    update();
}

bool MemeTile::isPlaying() const
{
    return m_movie && m_movie->state() == QMovie::Running;
}

void MemeTile::startMovie(const QString &path, const QSize &scaled)
{
    if (m_movie || path.isEmpty() || !m_animated)
        return;

    auto *movie = new QMovie(path, QByteArray(), this);   // parent=tile：瓦片析构时兜底
    if (!movie->isValid() || movie->frameCount() == 0) {
        // 动图解不开就明确说出来：静态海报还在，用户至少知道发生了什么。
        m_failure = QStringLiteral("动图解码失败：%1").arg(movie->lastErrorString());
        m_animated = false;
        delete movie;
        update();
        return;
    }

    // CacheNone：GIF 从文件流式读，QMovie 自己会 seek，不需要缓存全部帧。
    // 用 CacheAll 的话，2x DPR 下一个 30 帧的 GIF 能吃掉十几 MB，16 个瓦片就爆内存。
    movie->setCacheMode(QMovie::CacheNone);
    movie->setScaledSize(scaled);

    // 只 update() 不 repaint()：repaint 是同步重绘，滚动时会明显掉帧。
    connect(movie, &QMovie::frameChanged, this, [this](int) { update(); });
    connect(movie, &QMovie::error, this, [this](QImageReader::ImageReaderError) {
        if (!m_movie)
            return;
        m_failure = QStringLiteral("动图播放中断：%1").arg(m_movie->lastErrorString());
        update();
    });

    m_movie = movie;
    movie->start();
}

void MemeTile::stopMovie()
{
    if (!m_movie)
        return;

    // 先断连接再 stop：stop() 会改状态，残留的排队回调可能落进半停状态。
    m_movie->disconnect(this);
    m_movie->stop();
    // 同步 delete 而不是 deleteLater：这里不在 frameChanged 的槽内，
    // delete 是安全的；用 deleteLater 会让"快速滚出再滚回"短暂出现两个播放器。
    delete m_movie;
    m_movie = nullptr;
    update();   // 落回首帧海报
}

QRect MemeTile::imageRect() const
{
    return QRect(kPad, kPad, kImageSize, kImageSize);
}

QRect MemeTile::titleRect() const
{
    return QRect(kPad, kPad + kImageSize + 4, kImageSize, kTitleHeight);
}

QRect MemeTile::previewBadgeRect() const
{
    const QRect image = imageRect();
    return QRect(image.right() - 44 - kPad, image.top() + kPad, 44, kBadgeHeight);
}

void MemeTile::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF card(0.5, 0.5, width() - 1.0, height() - 1.0);

    // 卡片底与边框全部走令牌：写死色值的话切到深色主题会留下一块白底。
    // 常态用 Border 而不是 BorderSubtle：浅色主题下很多表情包本身是白底，
    // 边框太弱会让卡片和面板背景糊成一片。
    painter.setPen(QPen(xtools::theme::color(m_hover ? xtools::theme::Color::BorderAccent
                                                    : xtools::theme::Color::Border),
                        1));
    painter.setBrush(xtools::theme::color(m_hover ? xtools::theme::Color::BgHover
                                                 : xtools::theme::Color::BgAlt));
    painter.drawRoundedRect(card, 8, 8);

    const QRect image = imageRect();

    if (m_movie) {
        // 播放中：每帧都从 QMovie 取，按逻辑矩形绘制，缩放电平由 painter 处理。
        painter.drawPixmap(image, m_movie->currentPixmap());
    } else if (!m_poster.isNull()) {
        // 保持宽高比、居中，不擦掉圆角（卡片底已经画好了）。
        const QSize scaled =
            m_poster.size().scaled(image.size(), Qt::KeepAspectRatio);
        const QRect target(image.center().x() - scaled.width() / 2,
                           image.center().y() - scaled.height() / 2,
                           scaled.width(), scaled.height());
        painter.drawPixmap(target, m_poster);
    } else {
        // 没有图：画占位或失败原因。绝不静默留白。
        painter.setPen(xtools::theme::color(m_failure.isEmpty()
                                                ? xtools::theme::Color::TextFaint
                                                : xtools::theme::Color::DangerText));
        QFont small = font();
        small.setPixelSize(11);
        painter.setFont(small);
        const QString text = m_failure.isEmpty() ? QStringLiteral("加载中…") : m_failure;
        const QFontMetrics metrics(small);
        const QString elided =
            metrics.elidedText(text, Qt::ElideRight, image.width() - 12);
        painter.drawText(image, Qt::AlignCenter, elided);
    }

    // 「GIF」角标：只标动图，静态图不加装饰。
    if (m_animated) {
        const QRect badge(image.left() + kPad, image.top() + kPad, 30, kBadgeHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(xtools::theme::color(xtools::theme::Color::TooltipBg));
        painter.drawRoundedRect(badge, 4, 4);
        QFont badgeFont = font();
        badgeFont.setPixelSize(10);
        badgeFont.setBold(true);
        painter.setFont(badgeFont);
        painter.setPen(xtools::theme::color(xtools::theme::Color::TooltipText));
        painter.drawText(badge, Qt::AlignCenter, QStringLiteral("GIF"));
    }

    // 悬停时露出的「预览」角标，避免用户只能靠双击去猜。图片都没拿到时不显示，
    // 免得点开一个空预览。
    if (m_hover && m_failure.isEmpty() && !m_poster.isNull()) {
        const QRect badge = previewBadgeRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(xtools::theme::color(xtools::theme::Color::TooltipBg));
        painter.drawRoundedRect(badge, 4, 4);
        QFont badgeFont = font();
        badgeFont.setPixelSize(10);
        painter.setFont(badgeFont);
        painter.setPen(xtools::theme::color(xtools::theme::Color::TooltipText));
        painter.drawText(badge, Qt::AlignCenter, QStringLiteral("预览"));
    }

    // 标题：单行省略。QLabel 不会自己截断，固定宽度的卡片里会直接溢出到邻居上。
    if (!m_item.title.isEmpty()) {
        QFont titleFont = font();
        titleFont.setPixelSize(12);
        painter.setFont(titleFont);
        painter.setPen(xtools::theme::color(xtools::theme::Color::TextSecond));
        const QFontMetrics metrics(titleFont);
        const QString elided =
            metrics.elidedText(m_item.title, Qt::ElideRight, titleRect().width());
        painter.drawText(titleRect(), Qt::AlignLeft | Qt::AlignVCenter, elided);
    }
}

void MemeTile::enterEvent(QEnterEvent *event)
{
    m_hover = true;
    update();
    QWidget::enterEvent(event);
}

void MemeTile::leaveEvent(QEvent *event)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(event);
}

void MemeTile::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (previewBadgeRect().contains(event->pos())) {
            if (onPreview)
                onPreview(m_index);
        } else if (onCopy) {
            onCopy(m_index);
        }
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void MemeTile::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && onPreview) {
        onPreview(m_index);
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void MemeTile::contextMenuEvent(QContextMenuEvent *event)
{
    if (onContextMenu)
        onContextMenu(m_index, event->globalPos());
}

} // namespace meme

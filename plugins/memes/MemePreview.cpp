#include "MemePreview.h"

#include "ThemePalette.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMovie>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

namespace meme {

namespace {

/// 卡片最大宽度。再宽就纯属浪费——表情包本身只有几百像素。
constexpr int kCardMaxWidth = 720;
/// 卡片四周留白（相对浮层），保证遮罩能露出来，看得出这是浮层。
constexpr int kCardMargin = 28;
/// 卡片里除图片以外的固定占用：标题行 + 元信息行 + 按钮行 + 间距。
constexpr int kChromeHeight = 116;
constexpr int kCardPadding = 16;

} // namespace

MemePreview::MemePreview(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("memePreview"));
    // 遮罩必须走样式表而不是 QPainter：@overlay 的值是 "rgba(255,255,255,235)"
    // 这种 CSS 语法，样式表能吃，而 xtools::theme::color() 内部是 QColor(QString)，
    // 解析不了 rgb(a) 写法会返回无效色 —— fillRect 一个无效色等于什么都没画，
    // 界面上就是一块黑底。
    // 用 rgba 的样式表背景既拿到正确颜色，也能和下层网格正确混合。
    setAttribute(Qt::WA_StyledBackground, true);
    xtools::theme::applyStyle(
        this, QStringLiteral("QWidget#memePreview{background:@overlay;}"));

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("memePreviewCard"));
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    xtools::theme::applyStyle(
        m_card,
        QStringLiteral("QWidget#memePreviewCard{background:@bg;border:1px solid @border;"
                       "border-radius:12px;}"
                       "QLabel#memePreviewTitle{font-size:14px;font-weight:600;color:@text;"
                       "background:transparent;}"
                       "QLabel#memePreviewMeta{font-size:12px;color:@text-faint;"
                       "background:transparent;}"
                       "QLabel#memePreviewImage{background:transparent;}"
                       "QPushButton#memePreviewClose{background:transparent;border:none;"
                       "color:@text-second;font-size:16px;padding:0;min-height:22px;"
                       "min-width:22px;max-width:22px;max-height:22px;}"
                       "QPushButton#memePreviewClose:hover{color:@text;}"));

    auto *cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
    cardLayout->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    m_title = new QLabel(m_card);
    m_title->setObjectName(QStringLiteral("memePreviewTitle"));
    m_title->setWordWrap(false);
    header->addWidget(m_title, 1);
    m_close = new QPushButton(QStringLiteral("✕"), m_card);
    m_close->setObjectName(QStringLiteral("memePreviewClose"));
    m_close->setCursor(Qt::PointingHandCursor);
    m_close->setToolTip(QStringLiteral("关闭（Esc）"));
    header->addWidget(m_close, 0);
    cardLayout->addLayout(header);

    m_image = new QLabel(m_card);
    m_image->setObjectName(QStringLiteral("memePreviewImage"));
    m_image->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(m_image, 0, Qt::AlignCenter);

    m_meta = new QLabel(m_card);
    m_meta->setObjectName(QStringLiteral("memePreviewMeta"));
    m_meta->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    cardLayout->addWidget(m_meta);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    m_copy = new QPushButton(QStringLiteral("复制到剪贴板"), m_card);
    m_copy->setProperty("accent", true);
    m_copy->setCursor(Qt::PointingHandCursor);
    m_save = new QPushButton(QStringLiteral("另存为…"), m_card);
    m_save->setCursor(Qt::PointingHandCursor);
    actions->addWidget(m_copy);
    actions->addWidget(m_save);
    actions->addStretch(1);
    cardLayout->addLayout(actions);

    connect(m_close, &QPushButton::clicked, this, [this] { closePreview(); });
    connect(m_copy, &QPushButton::clicked, this, [this] {
        if (onCopy)
            onCopy();
    });
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (onSaveAs)
            onSaveAs();
    });

    // Esc 关闭。用 QShortcut 而不是只靠 keyPressEvent：焦点可能落在卡片里的按钮上，
    // keyPressEvent 只在本控件有焦点、且事件没被子控件吃掉时才到得了这里。
    auto *escape = new QShortcut(QKeySequence::Cancel, this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { closePreview(); });

    hide();
}

QSize MemePreview::imageBoxSize() const
{
    const int cardWidth = qMin(kCardMaxWidth, width() - 2 * kCardMargin);
    const int cardHeight = height() - 2 * kCardMargin;
    const int boxWidth = qMax(160, cardWidth - 2 * kCardPadding);
    const int boxHeight = qMax(120, cardHeight - kChromeHeight);
    return QSize(boxWidth, boxHeight);
}

void MemePreview::updateTitleElide()
{
    if (!m_title)
        return;
    // QLabel 不会自己打点：wordWrap 关掉时它直接把超出的字裁掉，视觉上像被切断。
    // 标题长度不可控（发表情有些标题长得离谱），所以自己按当前宽度省略。
    const QFontMetrics metrics(m_title->font());
    m_title->setText(metrics.elidedText(m_titleText, Qt::ElideRight, m_title->width()));
    m_title->setToolTip(m_titleText);
}

void MemePreview::layoutCard()
{
    if (!m_card)
        return;

    // 图片框按「浮层高度 - 卡片内其余几行」算，卡片高度再由 adjustSize() 反推，
    // 这样图片最大、又不会把按钮挤出可视区。
    const QSize box = imageBoxSize();
    m_image->setFixedSize(box);
    // 标题宽度钉死：不钉住的话超长标题会把卡片撑宽，甚至顶出浮层。
    m_title->setFixedWidth(qMax(80, box.width() - 26));

    m_card->adjustSize();
    m_card->setGeometry((width() - m_card->width()) / 2,
                        qMax(kCardMargin, (height() - m_card->height()) / 2),
                        m_card->width(), m_card->height());

    updateTitleElide();
}

void MemePreview::openItem(const MemeItem &item, const QString &path, bool animated,
                           const QString &meta, const QString &busyText)
{
    m_titleText = item.title.isEmpty() ? QStringLiteral("预览") : item.title;
    m_title->setText(m_titleText);
    m_title->setToolTip(m_titleText);
    m_metaText = meta;
    m_meta->setText(meta);
    m_path = path;
    m_animated = animated;

    setGeometry(parentWidget() ? parentWidget()->rect() : geometry());
    layoutCard();

    // 先 show 再塞图：原图还没下来时只显示「正在加载…」，也必须让浮层先出现，
    // 否则用户点了预览会觉得没反应。
    raise();
    show();
    setFocus();

    if (path.isEmpty()) {
        setMessage(busyText.isEmpty() ? QStringLiteral("正在加载…") : busyText);
        return;
    }

    applyImage();
}

void MemePreview::setImagePath(const QString &path, bool animated, const QString &meta)
{
    m_metaText = meta;
    m_meta->setText(meta);

    if (!path.isEmpty()) {
        m_path = path;
        m_animated = animated;
        applyImage();
        return;
    }

    // path 为空表示"原图没拿到"，此时 meta 里已经写了原因，用的是缩略图 —— 别清空图片。
    if (!m_path.isEmpty())
        return;

    setMessage(meta);
}

void MemePreview::setMessage(const QString &text)
{
    stopMovie();
    m_image->clear();
    m_image->setText(text);
}

void MemePreview::applyImage()
{
    stopMovie();
    m_image->clear();

    if (m_path.isEmpty())
        return;

    const qreal dpr = devicePixelRatioF();
    const QSize box = m_image->size();
    m_appliedBox = box;

    QImageReader probe(m_path);
    QSize source = probe.size();
    if (!source.isValid() || source.isEmpty())
        source = QSize(box.width(), box.height());   // 拿不到原始尺寸就按框来，至少不崩

    const QSize fitted = source.scaled(box, Qt::KeepAspectRatio);
    if (fitted.isEmpty())
        return;

    if (m_animated) {
        auto *movie = new QMovie(m_path, QByteArray(), this);
        if (movie->isValid() && movie->frameCount() > 0) {
            movie->setCacheMode(QMovie::CacheNone);
            // 按设备像素放大，2x 屏上才不糊。
            movie->setScaledSize(QSize(qRound(fitted.width() * dpr),
                                       qRound(fitted.height() * dpr)));
            m_image->setMovie(movie);
            m_movie = movie;
            movie->start();
            return;
        }
        delete movie;   // 解不开就退回静态首帧，下面接着走
    }

    QImage image(m_path);
    if (image.isNull()) {
        setMessage(QStringLiteral("这张图解码失败"));
        return;
    }
    const QSize target(qRound(fitted.width() * dpr), qRound(fitted.height() * dpr));
    QPixmap pixmap = QPixmap::fromImage(
        image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(dpr);
    m_image->setPixmap(pixmap);
}

void MemePreview::stopMovie()
{
    if (!m_movie)
        return;
    // 必须先让 QLabel 松手：它持有 QMovie 指针，直接 delete 会留下悬空引用，
    // 下一次重绘就崩在 label 内部。
    m_image->setMovie(nullptr);
    m_movie->disconnect(this);
    m_movie->stop();
    delete m_movie;
    m_movie = nullptr;
}

void MemePreview::closePreview()
{
    if (!isVisible() && m_path.isEmpty())
        return;

    stopMovie();
    m_image->clear();
    m_path.clear();
    m_appliedBox = QSize();
    hide();

    if (onClosed)
        onClosed();
}

void MemePreview::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutCard();
    // 只有图片框真的换了尺寸才重新解码/重建 QMovie：拖动窗口时 resizeEvent 会
    // 连续触发，每次都重建播放器会闪而且白烧 CPU。
    if (isVisible() && !m_path.isEmpty() && m_image->size() != m_appliedBox)
        applyImage();
}

void MemePreview::mousePressEvent(QMouseEvent *event)
{
    // 点遮罩关掉；点卡片内不关（卡片本身会吃掉自己的鼠标事件）。
    if (!m_card || !m_card->geometry().contains(event->pos())) {
        closePreview();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MemePreview::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        closePreview();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace meme

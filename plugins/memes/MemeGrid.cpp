#include "MemeGrid.h"

#include "ThemePalette.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace meme {

namespace {

/// 瓦片间距。取值不影响瓦片尺寸（瓦片是固定大小的），只影响一行能放几列。
constexpr int kGap = 10;

} // namespace

MemeGrid::MemeGrid(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    m_gridHost = new QWidget(this);
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(kGap);
    m_grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // AlignLeft|AlignTop：格子不被拉伸，瓦片保持固定尺寸、左对齐。
    root->addWidget(m_gridHost, 0, Qt::AlignLeft | Qt::AlignTop);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->addStretch(1);
    m_more = new QPushButton(QStringLiteral("加载更多"), this);
    m_more->setCursor(Qt::PointingHandCursor);
    footer->addWidget(m_more);
    footer->addStretch(1);
    root->addLayout(footer);

    // 尾部弹簧：结果很少时网格贴顶，不把最后一行撑到窗口底部。
    root->addStretch(1);

    m_more->hide();

    // 点「加载更多」由面板决定走哪一页；网格只负责把点击转出去。
    connect(m_more, &QPushButton::clicked, this, [this] {
        if (onLoadMore)
            onLoadMore();
    });

    setEmptyText(QStringLiteral("输入关键词搜索表情包"),
                 QStringLiteral("单击复制到剪贴板，双击或点角标看大图"));
}

int MemeGrid::columnsFor(int availableWidth)
{
    if (availableWidth <= MemeTile::kTileWidth)
        return 1;
    return qMax(1, (availableWidth + kGap) / (MemeTile::kTileWidth + kGap));
}

MemeTile *MemeGrid::addTile(const MemeItem &item)
{
    auto *tile = new MemeTile(m_gridHost);
    tile->setItem(item, m_tiles.size());
    m_tiles.append(tile);
    relayout(true);
    return tile;
}

void MemeGrid::clearTiles()
{
    for (MemeTile *tile : m_tiles) {
        m_grid->removeWidget(tile);
        tile->deleteLater();
    }
    m_tiles.clear();
    m_columns = 0;
    setFooter(Footer::Hidden);
    update();
}

MemeTile *MemeGrid::tileAt(int index) const
{
    return (index >= 0 && index < m_tiles.size()) ? m_tiles.at(index) : nullptr;
}

void MemeGrid::setEmptyText(const QString &title, const QString &hint)
{
    m_emptyTitle = title;
    m_emptyHint = hint;
    update();
}

void MemeGrid::setFooter(Footer state, const QString &loadMoreText)
{
    if (!m_more)
        return;

    switch (state) {
    case Footer::Hidden:
        m_more->hide();
        break;
    case Footer::LoadMore:
        m_more->setText(loadMoreText.isEmpty() ? QStringLiteral("加载更多") : loadMoreText);
        m_more->setEnabled(true);
        m_more->show();
        break;
    case Footer::Loading:
        m_more->setText(QStringLiteral("正在加载…"));
        m_more->setEnabled(false);
        m_more->show();
        break;
    case Footer::NoMore:
        m_more->setText(QStringLiteral("没有更多了"));
        m_more->setEnabled(false);
        m_more->show();
        break;
    }
}

void MemeGrid::relayout(bool force)
{
    const int columns = columnsFor(width());
    if (!force && columns == m_columns)
        return;
    m_columns = columns;

    // 先全部摘掉再放回：QGridLayout 不会自己重排已存在的格子，
    // 不摘就改列数会留下空洞。
    for (MemeTile *tile : m_tiles)
        m_grid->removeWidget(tile);

    for (int i = 0; i < m_tiles.size(); ++i) {
        m_grid->addWidget(m_tiles.at(i), i / m_columns, i % m_columns,
                          Qt::AlignLeft | Qt::AlignTop);
        m_tiles.at(i)->show();
    }
}

void MemeGrid::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout(false);
}

void MemeGrid::paintEvent(QPaintEvent *)
{
    if (!m_tiles.isEmpty() || m_emptyTitle.isEmpty())
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QFont titleFont = font();
    titleFont.setPixelSize(14);
    QFont hintFont = font();
    hintFont.setPixelSize(12);

    const QRect area = rect().adjusted(0, 40, 0, 0);

    painter.setFont(titleFont);
    painter.setPen(xtools::theme::color(xtools::theme::Color::TextMuted));
    QRect titleRect = area;
    titleRect.setHeight(24);
    painter.drawText(titleRect, Qt::AlignHCenter | Qt::AlignTop, m_emptyTitle);

    if (!m_emptyHint.isEmpty()) {
        painter.setFont(hintFont);
        painter.setPen(xtools::theme::color(xtools::theme::Color::TextFaint));
        QRect hintRect = area;
        hintRect.setTop(titleRect.bottom() + 6);
        hintRect.setHeight(20);
        painter.drawText(hintRect, Qt::AlignHCenter | Qt::AlignTop, m_emptyHint);
    }
}

} // namespace meme

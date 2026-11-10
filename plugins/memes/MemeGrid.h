#pragma once

// ---------------------------------------------------------------------------
// 表情包网格容器：列数自适应、瓦片重排、空态文案、底部「加载更多」。
//
// 用 QGridLayout 而不是手工 setGeometry：宿主首页（HomePage::placeTiles）已经
// 验证过「先 removeWidget 再按新列数 addWidget」这套做法，行为可预期；手工排版
// 还要自己处理高度、间距、滚动内容尺寸，多一类 bug。
//
// 布局结构（三层，别合并）：
//   MemeGrid(QWidget, 自己画空态)
//     └─ QVBoxLayout
//          ├─ m_gridHost(QWidget, AlignLeft|AlignTop) ─ QGridLayout(瓦片)
//          └─ footer(QHBoxLayout 居中放「加载更多」按钮)
// m_gridHost 用 AlignLeft|AlignTop、不设 Expanding，格子就不会被拉开，
// 瓦片之间保持固定间距——这也让瓦片尺寸恒定，QMovie 的缩放尺寸只需算一次。
// ---------------------------------------------------------------------------

#include "MemeSource.h"
#include "MemeTile.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

class QGridLayout;
class QPushButton;

namespace meme {

class MemeGrid : public QWidget
{
public:
    /// 底部状态。Hidden 用于搜索结果为空（空态文案已经在网格中央说了）。
    enum class Footer
    {
        Hidden,
        LoadMore,
        Loading,
        NoMore,
    };

    explicit MemeGrid(QWidget *parent = nullptr);

    /// 追加一个瓦片并重排。返回的瓦片归 MemeGrid 所有，调用方只连回调。
    MemeTile *addTile(const MemeItem &item);

    void clearTiles();
    int tileCount() const { return m_tiles.size(); }
    MemeTile *tileAt(int index) const;
    QList<MemeTile *> tiles() const { return m_tiles; }
    int columnCount() const { return m_columns; }

    /// 空态文案：第一行大字、第二行提示。只在 tileCount()==0 时绘制。
    void setEmptyText(const QString &title, const QString &hint);
    void setFooter(Footer state, const QString &loadMoreText = QString());

    std::function<void()> onLoadMore;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void relayout(bool force);
    static int columnsFor(int availableWidth);

    QList<MemeTile *> m_tiles;
    QWidget *m_gridHost = nullptr;
    QGridLayout *m_grid = nullptr;
    QPushButton *m_more = nullptr;
    int m_columns = 0;
    QString m_emptyTitle;
    QString m_emptyHint;
};

} // namespace meme

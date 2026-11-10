#pragma once

// ---------------------------------------------------------------------------
// 网格里的单个表情包瓦片。
//
// 为什么自绘而不是 QLabel 拼出来：一个瓦片只占一个 QWidget。用 QLabel 的写法
// 每个瓦片要挂「图片 + 标题」两层子控件，40 个瓦片就是 80 个控件，滚动时的
// 布局开销和内存都不划算；而且卡片底、圆角、GIF 角标、悬停态本来就只能自绘。
//
// 刻意不带 Q_OBJECT：瓦片不需要信号槽，回调一律用 std::function，
// 省掉 moc，临时测试工程也能直接编译它（识别时用 dynamic_cast，不能用 qobject_cast）。
//
// 交互约定（面板只负责连回调，不干预瓦片的命中判定）：
//   左键单击    → onCopy（主操作，无延迟 —— 用户的心智模型是"点一下就能粘"）
//   左键点右上角「预览」角标 / 双击 → onPreview
//   右键        → onContextMenu(全局坐标)
// 注意：双击会先触发一次 onCopy（Qt 的 release 早于 dblclick，无法用零延迟同时区分）。
// 复制一次再多看一张大图不会造成困扰，比延迟 400ms 才复制好得多，故如此取舍。
// ---------------------------------------------------------------------------

#include "MemeSource.h"

#include <QPixmap>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>

class QMovie;

namespace meme {

class MemeTile : public QWidget
{
public:
    /// 瓦片固定尺寸。固定而不是按列宽拉伸，是因为 QMovie::setScaledSize 依赖它：
    /// 尺寸一变，所有正在播放的动图都要重设缩放甚至重新解码，拖窗口时会卡。
    static constexpr int kTileWidth = 152;
    static constexpr int kTileHeight = 184;
    static constexpr int kPad = 6;
    static constexpr int kImageSize = 140;
    static constexpr int kTitleHeight = 22;

    explicit MemeTile(QWidget *parent = nullptr);

    void setItem(const MemeItem &item, int index);
    const MemeItem &item() const { return m_item; }
    int index() const { return m_index; }

    /// 首帧海报（静态图就是整张图）。animated 只影响角标，不决定是否播放。
    void setPoster(const QPixmap &poster, bool animated);
    /// 图片拿不到时的占位文案（403 / 解码失败 / WebP 不支持…）。绝不静默留白。
    void setFailure(const QString &reason);
    /// 丢弃已解码的图，回到"只有路径"的状态。滚远了的瓦片用这个回收内存。
    void clearImage();

    bool hasImage() const { return !m_poster.isNull(); }
    bool isAnimated() const { return m_animated; }
    bool isPlaying() const;

    /// 开始播放动图。scaled 是**设备像素**尺寸（已按 DPR 放大），存进 QMovie 才清晰。
    void startMovie(const QString &path, const QSize &scaled);
    void stopMovie();

    std::function<void(int)> onCopy;
    std::function<void(int)> onPreview;
    std::function<void(int, const QPoint &)> onContextMenu;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QRect imageRect() const;
    QRect titleRect() const;
    QRect previewBadgeRect() const;

    MemeItem m_item;
    int m_index = -1;
    QPixmap m_poster;
    QMovie *m_movie = nullptr;
    QString m_failure;
    bool m_animated = false;
    bool m_hover = false;
};

} // namespace meme

#pragma once

// ---------------------------------------------------------------------------
// 纵向拼接的纯逻辑（不碰界面、不碰抓屏，可单独跑临时断言程序验证）。
//
// 滚动截图的原理：对同一块屏幕区域反复抓帧，相邻两帧之间内容只是「上下平移」。
// 只要把平移量算出来、再把新露出的那几行接上去，就得到一张长图。
//
// 两个必须处理的现实问题：
//   1. 用户可能先往下滚再往上滚。所以不能简单地「每帧往尾部追加」，而要记录
//      当前帧相对画布顶端的偏移，只在「确实露出新内容」时才拼接，
//      否则来回滚一次就会把同一段内容重复贴进长图。
//   2. 整幅逐像素比位移在 140ms 的采样周期里太浪费。这里固定采样
//      72 列 × 20 行，一屏文字足够区分 ±1px 的错位，单帧计算量只有几万次比较。
// ---------------------------------------------------------------------------

#include <QImage>
#include <QPainter>
#include <QVector>
#include <QtGlobal>

#include <cmath>

namespace stitch {

/// 匹配质量下限：采样点里至少 90% 的像素（容许 ±10 的通道误差）对得上，
/// 才认这个位移。低于它的最佳匹配基本是「两帧根本没关系」，不能拿来拼。
inline constexpr double kMinQuality = 0.90;

/// 小于这个像素数的位移不算滚动：鼠标滚轮一格的位移远大于它，而 1~2px 的
/// 抖动（动画、光标闪烁）应当被忽略。
inline constexpr int kMinShiftPx = 3;

/// 一次位移检测的结果。
struct Shift
{
    int    dy = 0;        ///< >0 表示内容整体上移（用户往下滚），<0 表示上移的反面
    double quality = 0.0; ///< 重叠区域中匹配上的采样点占比 [0,1]

    /// 是否可以拿来拼接。kMinShiftPx 也在这里把「没动」挡掉。
    bool usable() const { return std::abs(dy) >= kMinShiftPx && quality >= kMinQuality; }
};

/// 两个像素是否「看起来一样」。不做精确相等：屏幕上有抗锯齿、光标闪烁、
/// 视频动画，逐位相等会把整体匹配率压到很低，反而找不到真正的位移。
inline bool colorClose(QRgb a, QRgb b, int tolerance = 10)
{
    return std::abs(qRed(a) - qRed(b)) <= tolerance
        && std::abs(qGreen(a) - qGreen(b)) <= tolerance
        && std::abs(qBlue(a) - qBlue(b)) <= tolerance;
}

///
/// 求 next 相对 prev 的纵向位移：约定 next[y] ≈ prev[y + dy]。
///
/// dy > 0 —— 内容整体往上走了（用户往下滚动），next 的底部是新内容；
/// dy < 0 —— 内容整体往下走了（用户往上滚动），next 的顶部是新内容。
///
/// 只支持 32 位格式（ARGB32 / RGB32），调用方须先把帧统一成 ARGB32：
/// 逐像素比较走 constScanLine + QRgb，8 位索引或浮点格式在这里没有意义。
/// prev / next 尺寸不一致时返回 dy=0（视为不可用）。
///
inline Shift findVerticalShift(const QImage &prev, const QImage &next, int maxShift)
{
    Shift best;
    if (prev.isNull() || next.isNull() || prev.size() != next.size())
        return best;

    const int width = prev.width();
    const int height = prev.height();
    if (width < 8 || height < 8)
        return best;

    // 允许的最大位移：留出至少 20% 的重叠，否则「位移」只是巧合。
    maxShift = qBound(1, maxShift, qMax(1, height - height / 5));

    const int colStep = qMax(1, width / 72);
    const int rowSamples = 20;

    for (int dy = -maxShift; dy <= maxShift; ++dy) {
        if (dy == 0)
            continue;

        // 两帧重叠的行区间：next 的 y 对应 prev 的 y+dy，两者都必须落在图内。
        const int yStart = qMax(0, -dy);
        const int yEnd = qMin(height, height - dy);
        const int overlap = yEnd - yStart;
        if (overlap < qMax(24, height / 5))
            continue;

        const int rowStep = qMax(1, overlap / rowSamples);
        int total = 0;
        int matched = 0;
        for (int y = yStart; y < yEnd; y += rowStep) {
            const QRgb *cur = reinterpret_cast<const QRgb *>(next.constScanLine(y));
            const QRgb *old = reinterpret_cast<const QRgb *>(prev.constScanLine(y + dy));
            for (int x = 0; x < width; x += colStep) {
                ++total;
                if (colorClose(cur[x], old[x]))
                    ++matched;
            }
        }
        if (total == 0)
            continue;

        const double quality = double(matched) / double(total);
        // 同分时取 |dy| 更小的那个：大面积空白、等距分隔线这类重复图案会在
        // 多个位移上拿到同样高的分，位移越小越可能是真实的滚动量。
        const bool better = quality > best.quality + 1e-9
            || (std::abs(quality - best.quality) <= 1e-9 && std::abs(dy) < std::abs(best.dy));
        if (better) {
            best.dy = dy;
            best.quality = quality;
        }
    }
    return best;
}

///
/// 纵向画布：把每帧新露出的行按顺序收集起来，最后一次性合成。
///
/// 为什么不是每帧都往一张大图上 drawImage：长图可能有 2 万像素高，
/// 每帧重贴一次就是几十 MB 的拷贝，7Hz 下会明显卡顿。这里每帧只存一条
/// 「新行切片」（通常几十到几百 KB），合成只在结束时做一次。
///
class VerticalCanvas
{
public:
    /// 用第一帧初始化画布。maxHeight 是长图高度上限（像素），
    /// 超过后不再增长并置 limitReached()，让上层提示用户。
    bool begin(const QImage &firstFrame, int maxHeight)
    {
        if (firstFrame.isNull() || maxHeight <= 0)
            return false;
        m_parts.clear();
        m_parts.push_back(firstFrame);
        m_width = firstFrame.width();
        m_frameHeight = firstFrame.height();
        m_height = m_frameHeight;
        m_offset = 0;             // 当前帧顶端在画布坐标系里的位置
        m_maxHeight = maxHeight;
        m_limitReached = false;
        return true;
    }

    bool isStarted() const { return m_height > 0; }

    /// 是否已达高度上限。
    bool limitReached() const { return m_limitReached; }

    /// 当前画布高度（像素）。
    int height() const { return m_height; }

    /// 帧数（含第一帧），用于界面提示「已拼接 N 帧」。
    int frameCount() const { return m_parts.size(); }

    ///
    /// 把新的一帧并入画布。dy 来自 findVerticalShift()（next 相对上一帧的位移）。
    /// 返回是否真的加入了新内容 —— 用户往回滚到已截范围内时返回 false，
    /// 上层据此区分「在滚动」与「停在原地」。
    ///
    bool addFrame(const QImage &frame, int dy)
    {
        if (m_height <= 0 || frame.isNull())
            return false;
        if (frame.width() != m_width || frame.height() != m_frameHeight)
            return false;

        m_offset += dy;

        // 情况一：当前帧顶端跑到了画布上方 —— 用户往上滚，顶部露出新内容。
        if (m_offset < 0) {
            int rows = -m_offset;
            if (m_height + rows > m_maxHeight) {
                rows = m_maxHeight - m_height;
                m_limitReached = true;
            }
            bool added = false;
            if (rows > 0) {
                m_parts.prepend(frame.copy(0, 0, m_width, rows));
                m_height += rows;
                added = true;
            }
            // 无论是否加上，偏移都要复位：否已达到上限时 m_offset 一直为负，
            // 之后每帧都会重复尝试前插，把画布撑爆。
            m_offset = 0;
            return added;
        }

        // 情况二：当前帧底端超出了画布底端 —— 用户往下滚，底部露出新内容。
        const int needed = m_offset + m_frameHeight - m_height;
        if (needed <= 0)
            return false;   // 还在已截范围内，没有任何新内容

        int rows = needed;
        if (m_height + rows > m_maxHeight) {
            rows = m_maxHeight - m_height;
            m_limitReached = true;
        }
        if (rows <= 0)
            return false;

        const int sourceY = m_height - m_offset;   // 新内容在帧内的起始行
        m_parts.push_back(frame.copy(0, sourceY, m_width, rows));
        m_height += rows;
        return true;
    }

    /// 合成最终长图（ARGB32、dpr=1）。返回空图表示画布尚未初始化。
    QImage image() const
    {
        if (m_height <= 0 || m_width <= 0)
            return {};

        QImage canvas(m_width, m_height, QImage::Format_ARGB32);
        canvas.fill(Qt::black);
        QPainter painter(&canvas);
        int y = 0;
        for (const QImage &part : m_parts) {
            painter.drawImage(0, y, part);
            y += part.height();
        }
        painter.end();
        return canvas;
    }

private:
    QVector<QImage> m_parts;      ///< 从上到下的切片，第 0 片是第一帧
    int  m_width = 0;
    int  m_frameHeight = 0;
    int  m_height = 0;            ///< 画布总高
    int  m_offset = 0;            ///< 最新一帧顶端在画布里的 y
    int  m_maxHeight = 0;
    bool m_limitReached = false;
};

} // namespace stitch

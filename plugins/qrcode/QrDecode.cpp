#include "QrDecode.h"

// ---------------------------------------------------------------------------
// 解码流水线：
//   1. 灰度化 + 二值化（全局 Otsu；照片光照不均时再试一次"局部均值归一化"）
//   2. 找定位图形（行游程 1:1:3:1:1 + 垂直/水平交叉校验 + 聚类）
//   3. 三点定方向与版本，用第四个点（校正图形）做透视变换，逐模块采样
//   4. 读格式信息（两份副本按汉明距离取优）→ 容错等级 + 掩码
//   5. 反掩码取码字 → 反交织 → 逐块 RS 纠错
//   6. 解析位流（数字 / 字母数字 / 字节 / ECI）
//
// 一些刻意的取舍：
//   * 不用 QImage 的灰度转换函数，而是自己按 Rec.601 算：QImage::convertToFormat
//     在缩放过的图上会带一次内存拷贝，而这里本来就要逐像素扫。
//   * 二值化只有两个候选（全局 + 局部归一化），不做多尺度尝试：二维码是黑白
//     高对比图形，再多的候选只会拖慢速度，换不来识别率。
//   * 版本估计附近最多试 5 个值（±2）：模块尺寸的估计误差通常只差 1 版，
//     再远说明定位本身就不对，继续试只是浪费。
// ---------------------------------------------------------------------------

#include "QrGalois.h"
#include "QrLayout.h"
#include "QrTables.h"

#include <QByteArray>
#include <QPainter>
#include <QStringDecoder>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace qrcode {

namespace {

/// 解码前的最大边长。手机照片动辄 4000px，缩到 2000 以内既够分辨最小模块
/// （v40 在 2000px 下每模块仍有 11px），又把后面的逐像素扫描压到可接受范围。
constexpr int kMaxDecodeSide = 2000;

/// 格式信息允许的最大汉明距离。15 位 BCH 能纠 3 位，超过就认为读到的不是格式信息。
constexpr int kMaxFormatErrors = 3;

// ---------------------------------------------------------------------------
// 灰度 / 二值化
// ---------------------------------------------------------------------------

struct GrayImage
{
    int width = 0;
    int height = 0;
    QVector<quint8> pixels;
};

/// RGB → 灰度（Rec.601 整数近似）。透明像素先合成到白底：
/// 截图工具与网页导出的 PNG 经常带透明背景，直接取 RGB 会得到"黑底黑码"。
GrayImage toGray(const QImage &source)
{
    QImage image = source;

    // hasAlphaChannel 对 Format_RGB32 也返回 true（它有一个恒为 0xFF 的 alpha 字节），
    // 所以这里看的是"是否真的有半透明像素"，避免无谓的一次整图重绘。
    if (image.hasAlphaChannel()) {
        bool needsFlatten = false;
        for (int y = 0; y < image.height() && !needsFlatten; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(line[x]) < 255) {
                    needsFlatten = true;
                    break;
                }
            }
        }
        if (needsFlatten) {
            QImage flattened(image.size(), QImage::Format_RGB32);
            flattened.fill(Qt::white);
            QPainter painter(&flattened);
            painter.drawImage(0, 0, image);
            painter.end();
            image = flattened;
        }
    }

    if (qMax(image.width(), image.height()) > kMaxDecodeSide) {
        image = image.scaled(kMaxDecodeSide, kMaxDecodeSide,
                             Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (image.format() != QImage::Format_RGB32)
        image = image.convertToFormat(QImage::Format_RGB32);

    GrayImage gray;
    gray.width = image.width();
    gray.height = image.height();
    gray.pixels.resize(gray.width * gray.height);
    for (int y = 0; y < gray.height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < gray.width; ++x) {
            const QRgb rgb = line[x];
            gray.pixels[y * gray.width + x] = static_cast<quint8>(
                (qRed(rgb) * 77 + qGreen(rgb) * 150 + qBlue(rgb) * 29) >> 8);
        }
    }
    return gray;
}

/// Otsu 全局阈值：让"黑/白两类之间的方差"最大，等价于找一个不受极端像素影响的
/// 分界点。二维码只有两种颜色，这一步在正常截图上几乎总是对的。
QVector<quint8> binarizeOtsu(const GrayImage &gray)
{
    int histogram[256] = {0};
    for (const quint8 value : gray.pixels)
        ++histogram[value];

    const int total = gray.pixels.size();
    double sumAll = 0;
    for (int i = 0; i < 256; ++i)
        sumAll += double(i) * histogram[i];

    double sumBack = 0;
    int weightBack = 0;
    double bestVariance = -1;
    int threshold = 127;
    for (int t = 0; t < 256; ++t) {
        weightBack += histogram[t];
        if (weightBack == 0)
            continue;
        const int weightFore = total - weightBack;
        if (weightFore == 0)
            break;
        sumBack += double(t) * histogram[t];
        const double meanBack = sumBack / weightBack;
        const double meanFore = (sumAll - sumBack) / weightFore;
        const double variance = double(weightBack) * weightFore
                              * (meanBack - meanFore) * (meanBack - meanFore);
        if (variance > bestVariance) {
            bestVariance = variance;
            threshold = t;
        }
    }

    QVector<quint8> bin(total);
    for (int i = 0; i < total; ++i)
        bin[i] = gray.pixels[i] <= threshold ? 1 : 0;
    return bin;
}

/// 局部均值归一化后再 Otsu：等价于"把每个像素除以它周围一圈的平均亮度"。
/// 这样阴影、手挡住一半、屏幕反光造成的亮度梯度会被拉平，而二维码的黑白对比
/// 仍然保留。比直接做局部阈值稳：整块纯黑区域不会被误判成白（那是对着均值
/// 做比较时最常见的翻车点）。
QVector<quint8> binarizeNormalized(const GrayImage &gray)
{
    const int w = gray.width;
    const int h = gray.height;
    if (w < 8 || h < 8)
        return binarizeOtsu(gray);

    // 积分图：(w+1)*(h+1)，下标 (y+1)*(w+1)+(x+1) 存左上角 (0,0)~(x,y) 的和
    QVector<quint32> integral((w + 1) * (h + 1), 0);
    for (int y = 0; y < h; ++y) {
        quint32 rowSum = 0;
        for (int x = 0; x < w; ++x) {
            rowSum += gray.pixels[y * w + x];
            integral[(y + 1) * (w + 1) + (x + 1)] =
                integral[y * (w + 1) + (x + 1)] + rowSum;
        }
    }

    const int radius = qMax(8, qMin(w, h) / 12);
    GrayImage normalized;
    normalized.width = w;
    normalized.height = h;
    normalized.pixels.resize(w * h);
    for (int y = 0; y < h; ++y) {
        const int y0 = qMax(0, y - radius);
        const int y1 = qMin(h - 1, y + radius);
        for (int x = 0; x < w; ++x) {
            const int x0 = qMax(0, x - radius);
            const int x1 = qMin(w - 1, x + radius);
            const quint32 sum = integral[(y1 + 1) * (w + 1) + (x1 + 1)]
                              - integral[y0 * (w + 1) + (x1 + 1)]
                              - integral[(y1 + 1) * (w + 1) + x0]
                              + integral[y0 * (w + 1) + x0];
            const int count = (x1 - x0 + 1) * (y1 - y0 + 1);
            const int mean = qMax(1, int(sum) / count);
            // 以局部均值为基准放大到 0–255：均值处的像素落在 128，黑白自然分开
            const int scaled = qBound(0, gray.pixels[y * w + x] * 128 / mean, 255);
            normalized.pixels[y * w + x] = static_cast<quint8>(scaled);
        }
    }
    return binarizeOtsu(normalized);
}

// ---------------------------------------------------------------------------
// 定位图形
// ---------------------------------------------------------------------------

struct FinderPoint
{
    double x = 0;
    double y = 0;
    double module = 0;   ///< 单模块像素数（由 1:1:3:1:1 的游程长度估计）
    int count = 0;       ///< 命中次数：真实定位图形会被很多行扫到
};

struct Run
{
    int start = 0;
    int length = 0;
    bool black = false;
};

/// 1:1:3:1:1 比例检查。moduleSize 输出单模块像素数。
bool ratioMatches(const int counts[5], double &moduleSize)
{
    const int total = counts[0] + counts[1] + counts[2] + counts[3] + counts[4];
    if (total < 7)
        return false;
    moduleSize = double(total) / 7.0;
    // 容差取 0.5 个模块：再松会把数据区里 1:2:3:2:1 这类片段也当成定位图形
    // （密集码里它们会成片出现），代价是斜拍照片上稍微容易漏，靠后面的版本
    // 重试与透视校正补回来。
    const double maxVariance = moduleSize * 0.5;
    if (std::fabs(moduleSize - counts[0]) > maxVariance)
        return false;
    if (std::fabs(moduleSize - counts[1]) > maxVariance)
        return false;
    if (std::fabs(3.0 * moduleSize - counts[2]) > 3.0 * maxVariance)
        return false;
    if (std::fabs(moduleSize - counts[3]) > maxVariance)
        return false;
    if (std::fabs(moduleSize - counts[4]) > maxVariance)
        return false;
    return true;
}

/// 在候选列上做垂直方向的 1:1:3:1:1 复核，并把中心修到子像素精度。
///
/// @param maxCount      水平扫描时中间黑段的长度（3 个模块），用作向上/向下的搜索上限
/// @param originalTotal 水平扫描 5 段的总长度，用于"垂直方向宽度应当差不多"的校验
///
bool crossCheckVertical(const QVector<quint8> &bin, int w, int h,
                        int x, int yStart, int maxCount, int originalTotal,
                        double &centerY, double &moduleOut)
{
    if (x < 0 || x >= w || yStart < 0 || yStart >= h)
        return false;

    int counts[5] = {0, 0, 0, 0, 0};

    int y = yStart;
    while (y >= 0 && bin[y * w + x]) {
        ++counts[2];
        --y;
    }
    if (y < 0)
        return false;
    while (y >= 0 && !bin[y * w + x] && counts[1] <= maxCount) {
        ++counts[1];
        --y;
    }
    if (y < 0 || counts[1] > maxCount)
        return false;
    while (y >= 0 && bin[y * w + x] && counts[0] <= maxCount) {
        ++counts[0];
        --y;
    }
    if (counts[0] > maxCount)
        return false;

    y = yStart + 1;
    while (y < h && bin[y * w + x]) {
        ++counts[2];
        ++y;
    }
    if (y >= h)
        return false;
    while (y < h && !bin[y * w + x] && counts[3] <= maxCount) {
        ++counts[3];
        ++y;
    }
    if (y >= h || counts[3] > maxCount)
        return false;
    while (y < h && bin[y * w + x] && counts[4] <= maxCount) {
        ++counts[4];
        ++y;
    }
    if (counts[4] > maxCount)
        return false;

    const int total = counts[0] + counts[1] + counts[2] + counts[3] + counts[4];
    // 垂直方向的总宽度与水平方向相差不能超过 40%，否则这是别的图案
    if (5 * qAbs(total - originalTotal) >= 2 * originalTotal)
        return false;
    if (!ratioMatches(counts, moduleOut))
        return false;

    centerY = double(y - counts[4] - counts[3]) - counts[2] / 2.0;
    return true;
}

/// 水平方向复核（与上面镜像），用于把中心 x 修准。
bool crossCheckHorizontal(const QVector<quint8> &bin, int w, int h,
                          int xStart, int y, int maxCount, int originalTotal,
                          double &centerX, double &moduleOut)
{
    if (y < 0 || y >= h || xStart < 0 || xStart >= w)
        return false;

    int counts[5] = {0, 0, 0, 0, 0};

    int x = xStart;
    while (x >= 0 && bin[y * w + x]) {
        ++counts[2];
        --x;
    }
    if (x < 0)
        return false;
    while (x >= 0 && !bin[y * w + x] && counts[1] <= maxCount) {
        ++counts[1];
        --x;
    }
    if (x < 0 || counts[1] > maxCount)
        return false;
    while (x >= 0 && bin[y * w + x] && counts[0] <= maxCount) {
        ++counts[0];
        --x;
    }
    if (counts[0] > maxCount)
        return false;

    x = xStart + 1;
    while (x < w && bin[y * w + x]) {
        ++counts[2];
        ++x;
    }
    if (x >= w)
        return false;
    while (x < w && !bin[y * w + x] && counts[3] <= maxCount) {
        ++counts[3];
        ++x;
    }
    if (x >= w || counts[3] > maxCount)
        return false;
    while (x < w && bin[y * w + x] && counts[4] <= maxCount) {
        ++counts[4];
        ++x;
    }
    if (counts[4] > maxCount)
        return false;

    const int total = counts[0] + counts[1] + counts[2] + counts[3] + counts[4];
    if (5 * qAbs(total - originalTotal) >= 2 * originalTotal)
        return false;
    if (!ratioMatches(counts, moduleOut))
        return false;

    centerX = double(x - counts[4] - counts[3]) - counts[2] / 2.0;
    return true;
}

/// 全图扫描定位图形。返回按命中次数聚类的候选点（同一图形会被几十行扫到，
/// 必须合并，否则一个图形会被当成十几个候选）。
QVector<FinderPoint> findFinderPatterns(const QVector<quint8> &bin, int w, int h)
{
    QVector<FinderPoint> candidates;
    QVector<Run> runs;

    for (int y = 0; y < h; ++y) {
        // 逐行建游程。runs 在循环外声明并 clear()，避免每行都重新分配内存
        runs.clear();
        int x = 0;
        while (x < w) {
            const bool black = bin[y * w + x] != 0;
            const int start = x;
            while (x < w && (bin[y * w + x] != 0) == black)
                ++x;
            runs.append({start, x - start, black});
        }

        for (int i = 0; i + 4 < runs.size(); ++i) {
            // 游程天然黑白交替：i 为黑就保证 i+1/i+3 是白、i+2/i+4 是黑
            if (!runs[i].black)
                continue;

            int counts[5];
            for (int k = 0; k < 5; ++k)
                counts[k] = runs[i + k].length;

            double moduleSize = 0;
            if (!ratioMatches(counts, moduleSize))
                continue;

            const int originalTotal = counts[0] + counts[1] + counts[2] + counts[3] + counts[4];
            const int centerX = runs[i + 2].start + runs[i + 2].length / 2;

            double centerY = 0;
            double verticalModule = 0;
            if (!crossCheckVertical(bin, w, h, centerX, y, counts[2], originalTotal,
                                    centerY, verticalModule))
                continue;

            double refinedX = 0;
            double horizontalModule = 0;
            if (!crossCheckHorizontal(bin, w, h, centerX, qRound(centerY), counts[2],
                                      originalTotal, refinedX, horizontalModule))
                continue;

            const double measuredModule =
                (moduleSize + verticalModule + horizontalModule) / 3.0;

            bool merged = false;
            for (FinderPoint &point : candidates) {
                const double tolerance = qMax(2.0, point.module * 2.0);
                if (std::fabs(point.x - refinedX) <= tolerance
                    && std::fabs(point.y - centerY) <= tolerance) {
                    const int total = point.count + 1;
                    point.x = (point.x * point.count + refinedX) / total;
                    point.y = (point.y * point.count + centerY) / total;
                    point.module = (point.module * point.count + measuredModule) / total;
                    point.count = total;
                    merged = true;
                    break;
                }
            }
            if (!merged)
                candidates.append({refinedX, centerY, measuredModule, 1});
        }
    }

    // 只被扫到一行的候选基本是噪声（真实定位图形纵向至少覆盖 3 个模块）
    QVector<FinderPoint> filtered;
    filtered.reserve(candidates.size());
    for (const FinderPoint &point : candidates) {
        if (point.count >= 2)
            filtered.append(point);
    }
    std::sort(filtered.begin(), filtered.end(),
              [](const FinderPoint &a, const FinderPoint &b) { return a.count > b.count; });
    return filtered;
}

double distanceBetween(const FinderPoint &a, const FinderPoint &b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

/// 三个定位图形的组合。tl 是左上角，tr / bl 由叉积定向。
struct FinderTriple
{
    FinderPoint tl;
    FinderPoint tr;
    FinderPoint bl;
    double score = 0;
};

////
/// 枚举所有"像三个定位图形"的组合，按可信度排序返回。
///
/// 为什么不是只挑一个：密集的大版本里，数据区会出现大量与定位图形同构的
/// 1:1:3:1:1 片段（重复字符产生的规律码流尤其明显），它们被扫到的行数甚至
/// 比真定位图形还多。只按命中次数取前三会直接把真定位图形挤出去，所以这里把
/// 几何上说得通的组合全列出来，交给上层逐个"试着解" —— 格式信息与 RS 校验
/// 是比任何启发式都可靠的判据。
///
/// 几何判据：三点构成等腰直角三角形（两边比 < 1.6、勾股误差 < 30%、
/// 模块尺寸比 < 1.8）。得分里加上斜边权重的理由：真正的三个定位图形在整张
/// 码的角上，跨度最大，而数据区里的同构片段彼此靠得很近。
///
////
/// 从海量候选里挑一个"代表集"交给组合枚举。
///
/// 不能简单按命中次数取前 N：密集的大版本里数据区布满与定位图形同构的片段，
/// 它们会被更多扫描行命中，反而把真定位图形挤出去。这里按三种互补的特征各取
/// 一批，保证真定位图形一定在集合里：
///   1. 命中次数最多的几个（普通图片下它们就是真定位图形）；
///   2. 模块尺寸最小的几个（数据区的假片段往往由 1:2:3:2:1 拼出来，尺寸偏大）；
///   3. 四个方向上的极值点（真定位图形就在码的三个角上）。
///
QVector<FinderPoint> shortlistCandidates(const QVector<FinderPoint> &points)
{
    QVector<FinderPoint> result;
    const auto add = [&](const FinderPoint &point) {
        for (const FinderPoint &existing : result) {
            if (std::fabs(existing.x - point.x) < 1.0
                && std::fabs(existing.y - point.y) < 1.0)
                return;
        }
        result.append(point);
    };

    const int byCount = qMin(points.size(), 8);
    for (int i = 0; i < byCount; ++i)
        add(points[i]);

    QVector<FinderPoint> bySize = points;
    std::sort(bySize.begin(), bySize.end(),
              [](const FinderPoint &a, const FinderPoint &b) { return a.module < b.module; });
    for (int i = 0; i < qMin(bySize.size(), 5); ++i)
        add(bySize[i]);

    // 四个角方向：x+y 最小/最大、x-y 最小/最大
    const auto addExtreme = [&](int metric) {
        const FinderPoint *best = nullptr;
        double bestValue = 0;
        for (const FinderPoint &point : points) {
            const double value = metric == 0 ? point.x + point.y
                               : metric == 1 ? point.x - point.y
                               : metric == 2 ? -point.x + point.y
                                             : -(point.x + point.y);
            if (!best || value > bestValue) {
                best = &point;
                bestValue = value;
            }
        }
        if (best)
            add(*best);
    };
    for (int metric = 0; metric < 4; ++metric)
        addExtreme(metric);

    return result;
}

QVector<FinderTriple> rankTriples(const QVector<FinderPoint> &points)
{
    QVector<FinderTriple> triples;
    const QVector<FinderPoint> shortlist = shortlistCandidates(points);
    const int n = shortlist.size();

    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            for (int c = b + 1; c < n; ++c) {
                const FinderPoint *p = &shortlist[a];
                const FinderPoint *q = &shortlist[b];
                const FinderPoint *r = &shortlist[c];

                const double dab = distanceBetween(*p, *q);
                const double dac = distanceBetween(*p, *r);
                const double dbc = distanceBetween(*q, *r);

                // 最长边是斜边，它的两个端点就是 tr / bl，剩下那个是 tl
                const FinderPoint *tl = nullptr;
                const FinderPoint *u = nullptr;
                const FinderPoint *v = nullptr;
                double diagonal = 0;
                if (dab >= dac && dab >= dbc) {
                    tl = r; u = p; v = q; diagonal = dab;
                } else if (dac >= dab && dac >= dbc) {
                    tl = q; u = p; v = r; diagonal = dac;
                } else {
                    tl = p; u = q; v = r; diagonal = dbc;
                }

                const double leg1 = distanceBetween(*tl, *u);
                const double leg2 = distanceBetween(*tl, *v);
                if (leg1 < 8 || leg2 < 8)
                    continue;

                const double ratio = qMax(leg1, leg2) / qMin(leg1, leg2);
                if (ratio > 1.6)
                    continue;

                const double expected = leg1 * leg1 + leg2 * leg2;
                if (std::fabs(diagonal * diagonal - expected) > 0.3 * expected)
                    continue;

                const double moduleMax = qMax(tl->module, qMax(u->module, v->module));
                const double moduleMin = qMin(tl->module, qMin(u->module, v->module));
                if (moduleMin <= 0 || moduleMax / moduleMin > 1.8)
                    continue;

                // 叉积定向：图像坐标 y 向下，标准方向下 (tr-tl)×(bl-tl) > 0
                const double cross = (u->x - tl->x) * (v->y - tl->y)
                                   - (u->y - tl->y) * (v->x - tl->x);

                FinderTriple triple;
                triple.tl = *tl;
                triple.tr = cross > 0 ? *u : *v;
                triple.bl = cross > 0 ? *v : *u;
                // 排序以"跨度"为主：真正的三个定位图形在整张码的三个角上，
                // 两两距离必然最大；命中次数只作微调（密集码里假的同构片段反而
                // 可能被更多扫描行命中，单看次数会把真定位图形挤到后面）。
                triple.score = diagonal
                             + 0.05 * (tl->count + u->count + v->count)
                             - 50.0 * (ratio - 1.0);
                triples.append(triple);
            }
        }
    }

    std::sort(triples.begin(), triples.end(),
              [](const FinderTriple &a, const FinderTriple &b) {
                  return a.score > b.score;
              });
    return triples;
}

// ---------------------------------------------------------------------------
// 透视变换
// ---------------------------------------------------------------------------

/// 3×3 单应矩阵（h[8] 固定为 1）。
struct ProjectiveMap
{
    double h[8] = {1, 0, 0, 0, 1, 0, 0, 0};
    bool valid = false;

    QPointF map(double x, double y) const
    {
        const double denominator = h[6] * x + h[7] * y + 1.0;
        if (std::fabs(denominator) < 1e-12)
            return QPointF(-1, -1);
        return QPointF((h[0] * x + h[1] * y + h[2]) / denominator,
                       (h[3] * x + h[4] * y + h[5]) / denominator);
    }
};

/// 四点求单应（高斯消元解 8×8）。四个点不能有任意三点共线。
bool solveHomography(const QPointF src[4], const QPointF dst[4], ProjectiveMap &out)
{
    double a[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const double x = src[i].x();
        const double y = src[i].y();
        const double u = dst[i].x();
        const double v = dst[i].y();

        double *r0 = a[2 * i];
        r0[0] = x; r0[1] = y; r0[2] = 1.0;
        r0[6] = -x * u; r0[7] = -y * u; r0[8] = u;

        double *r1 = a[2 * i + 1];
        r1[3] = x; r1[4] = y; r1[5] = 1.0;
        r1[6] = -x * v; r1[7] = -y * v; r1[8] = v;
    }

    for (int col = 0; col < 8; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 8; ++row) {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col]))
                pivot = row;
        }
        if (std::fabs(a[pivot][col]) < 1e-9)
            return false;   // 退化（三点共线 / 定位点重合）
        if (pivot != col) {
            for (int k = col; k < 9; ++k)
                std::swap(a[col][k], a[pivot][k]);
        }
        const double divisor = a[col][col];
        for (int k = col; k < 9; ++k)
            a[col][k] /= divisor;
        for (int row = 0; row < 8; ++row) {
            if (row == col)
                continue;
            const double factor = a[row][col];
            if (factor == 0)
                continue;
            for (int k = col; k < 9; ++k)
                a[row][k] -= factor * a[col][k];
        }
    }

    for (int i = 0; i < 8; ++i)
        out.h[i] = a[i][8];
    out.valid = true;
    return true;
}

/// 只用三个定位图形的仿射近似（没有找到校正图形时的兜底）。
/// 二维码是平面图形，没有透视时这就是精确解；有透视时误差集中在右下角，
/// 小版本（v1）没有校正图形，只能靠它。
ProjectiveMap affineMap(const FinderPoint &tl, const FinderPoint &tr,
                        const FinderPoint &bl, int dimension)
{
    const double span = double(dimension - 7);   // 两个定位图形中心相距 dimension-7 个模块
    const double ux = (tr.x - tl.x) / span;
    const double uy = (tr.y - tl.y) / span;
    const double vx = (bl.x - tl.x) / span;
    const double vy = (bl.y - tl.y) / span;

    ProjectiveMap map;
    map.h[0] = ux;
    map.h[1] = vx;
    map.h[2] = tl.x - 3.5 * ux - 3.5 * vx;
    map.h[3] = uy;
    map.h[4] = vy;
    map.h[5] = tl.y - 3.5 * uy - 3.5 * vy;
    map.h[6] = 0;
    map.h[7] = 0;
    map.valid = true;
    return map;
}

////
/// 在预测位置附近找校正图形（5×5 的黑框 + 白环 + 黑心）。
///
/// 用模板打分而不是找游程：校正图形太小（5 模块），用比例法容易和普通数据
/// 模块混淆；这里直接按"应该黑/应该白"的 25 个采样点统计匹配数，
/// 达到 22/25 才认。
///
bool findAlignment(const QVector<quint8> &bin, int w, int h,
                   const QPointF &predicted, double moduleSize, QPointF &center)
{
    const int radius = qMax(3, int(moduleSize * 4));
    const int step = qMax(1, int(moduleSize / 2));

    int bestScore = 0;
    for (int dy = -radius; dy <= radius; dy += step) {
        for (int dx = -radius; dx <= radius; dx += step) {
            const double cx = predicted.x() + dx;
            const double cy = predicted.y() + dy;

            int score = 0;
            for (int my = -2; my <= 2; ++my) {
                for (int mx = -2; mx <= 2; ++mx) {
                    const int px = qRound(cx + mx * moduleSize);
                    const int py = qRound(cy + my * moduleSize);
                    if (px < 0 || py < 0 || px >= w || py >= h)
                        continue;
                    // 校正图形：外框与中心是黑，半径 1 的环是白
                    const bool expectedDark = qMax(qAbs(mx), qAbs(my)) != 1;
                    if ((bin[py * w + px] != 0) == expectedDark)
                        ++score;
                }
            }
            if (score > bestScore) {
                bestScore = score;
                center = QPointF(cx, cy);
            }
        }
    }
    if (bestScore < 22)
        return false;

    // 粗搜的步长是半个模块，落到整数像素上最多偏半个模块；直接用这个位置做透视
    // 会把误差放大到整个右下角（实测 v2 偏 3px 就足以让格式信息读错）。
    //
    // 精修办法：校正图形的中心是一个 1 模块的黑方块、外面紧贴一圈白，所以从
    // 粗定位点附近找到一个黑像素，再做**连通域**填充，得到的就是那个黑方块本身
    // （白环天然把它隔开）。取连通域的质心，比在固定窗口里统计黑像素稳：
    // 固定窗口在模块较大时会蹭到外框，把中心拉偏（12px 模块时偏差可达 3px）。
    const int searchRadius = qMax(2, int(moduleSize * 1.2));
    int seedX = -1;
    int seedY = -1;
    double seedDistance = 1e18;
    for (int dy = -searchRadius; dy <= searchRadius; ++dy) {
        for (int dx = -searchRadius; dx <= searchRadius; ++dx) {
            const int px = qRound(center.x()) + dx;
            const int py = qRound(center.y()) + dy;
            if (px < 0 || py < 0 || px >= w || py >= h || !bin[py * w + px])
                continue;
            const double distance = double(dx) * dx + double(dy) * dy;
            if (distance < seedDistance) {
                seedDistance = distance;
                seedX = px;
                seedY = py;
            }
        }
    }

    if (seedX >= 0) {
        // 连通域搜索限制在 2 个模块的方框内：中心方块最多这么大，
        // 再往外就是外框，不该算进质心。
        const int boxRadius = qMax(2, int(moduleSize * 2.0));
        const int x0 = qMax(0, seedX - boxRadius);
        const int y0 = qMax(0, seedY - boxRadius);
        const int x1 = qMin(w - 1, seedX + boxRadius);
        const int y1 = qMin(h - 1, seedY + boxRadius);
        const int boxWidth = x1 - x0 + 1;

        QVector<quint8> visited(boxWidth * (y1 - y0 + 1), 0);
        QVector<QPoint> stack;
        stack.append(QPoint(seedX, seedY));
        visited[(seedY - y0) * boxWidth + (seedX - x0)] = 1;

        double sumX = 0;
        double sumY = 0;
        int count = 0;
        while (!stack.isEmpty()) {
            const QPoint point = stack.takeLast();
            sumX += point.x() + 0.5;
            sumY += point.y() + 0.5;
            ++count;
            const int neighbours[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &offset : neighbours) {
                const int nx = point.x() + offset[0];
                const int ny = point.y() + offset[1];
                if (nx < x0 || ny < y0 || nx > x1 || ny > y1)
                    continue;
                const int index = (ny - y0) * boxWidth + (nx - x0);
                if (visited[index] || !bin[ny * w + nx])
                    continue;
                visited[index] = 1;
                stack.append(QPoint(nx, ny));
            }
        }

        // 面积应当是一个模块量级；跑飞了（连到外框或大片黑区）就宁可保留粗定位
        const double expected = moduleSize * moduleSize;
        if (count >= 2 && count <= int(expected * 2.5))
            center = QPointF(sumX / count, sumY / count);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 纠错（Reed-Solomon 解码）
// ---------------------------------------------------------------------------

/// 在 GF(256) 上纠正一个块。ecCount 是每块纠错码字数，block 原地修改。
///
/// 流程：求伴随式 → Berlekamp–Massey 求错误定位多项式 → Chien 搜索找位置
/// → Forney 公式求错误值。最后**重新算一遍伴随式**验证：如果还有非零伴随式，
/// 说明错误数超过 t = ecCount/2，绝不能把"改了一半"的数据当成成功。
bool reedSolomonDecode(QVector<quint8> &block, int ecCount, int &corrected)
{
    corrected = 0;
    const int n = block.size();
    if (ecCount <= 0 || n <= ecCount)
        return false;

    const auto syndromesAreZero = [&](QVector<quint8> *out) {
        bool allZero = true;
        if (out)
            out->resize(ecCount);
        for (int i = 0; i < ecCount; ++i) {
            const quint8 x = qrgalois::expOf(i);
            quint8 value = 0;
            for (int j = 0; j < n; ++j)
                value = static_cast<quint8>(qrgalois::multiply(value, x) ^ block[j]);
            if (out)
                (*out)[i] = value;
            if (value != 0)
                allZero = false;
        }
        return allZero;
    };

    QVector<quint8> synd;
    if (syndromesAreZero(&synd))
        return true;   // 没有错误，连纠错都不用做

    // ---- Berlekamp–Massey ----
    QVector<quint8> lambda(ecCount + 1, 0);
    QVector<quint8> previous(ecCount + 1, 0);
    lambda[0] = 1;
    previous[0] = 1;

    int errorCount = 0;            // L
    int shift = 1;                 // m
    quint8 lastDiscrepancy = 1;    // b

    for (int k = 0; k < ecCount; ++k) {
        quint8 discrepancy = synd[k];
        for (int i = 1; i <= errorCount; ++i)
            discrepancy = static_cast<quint8>(
                discrepancy ^ qrgalois::multiply(lambda[i], synd[k - i]));

        if (discrepancy == 0) {
            ++shift;
            continue;
        }

        const quint8 factor = qrgalois::divide(discrepancy, lastDiscrepancy);
        const QVector<quint8> saved = lambda;
        for (int i = 0; i + shift <= ecCount; ++i) {
            lambda[i + shift] = static_cast<quint8>(
                lambda[i + shift] ^ qrgalois::multiply(factor, previous[i]));
        }

        if (2 * errorCount <= k) {
            errorCount = k + 1 - errorCount;
            previous = saved;
            lastDiscrepancy = discrepancy;
            shift = 1;
        } else {
            ++shift;
        }
    }

    if (errorCount <= 0 || 2 * errorCount > ecCount)
        return false;

    // ---- Chien 搜索 ----
    // Λ(α^-i) = 0 ⇒ 错误位置在码字第 (n-1-i) 个字节（第一个字节对应最高次项）
    QVector<int> positions;
    QVector<quint8> powers;
    for (int i = 0; i < n; ++i) {
        const quint8 xInverse = qrgalois::expOf(-i);
        quint8 value = 0;
        for (int j = errorCount; j >= 0; --j)
            value = static_cast<quint8>(
                qrgalois::multiply(value, xInverse) ^ lambda[j]);
        if (value != 0)
            continue;
        const int index = n - 1 - i;
        if (index < 0)
            return false;
        positions.append(index);
        powers.append(qrgalois::expOf(i));
    }
    if (positions.size() != errorCount)
        return false;

    // ---- Ω(x) = S(x)·Λ(x) mod x^ecCount ----
    QVector<quint8> omega(ecCount, 0);
    for (int i = 0; i < ecCount; ++i) {
        if (synd[i] == 0)
            continue;
        for (int j = 0; j <= errorCount && i + j < ecCount; ++j) {
            if (lambda[j] == 0)
                continue;
            omega[i + j] = static_cast<quint8>(
                omega[i + j] ^ qrgalois::multiply(synd[i], lambda[j]));
        }
    }

    // ---- Forney：e_i = Ω(X_i^-1) / Π_{j≠i}(1 + X_j/X_i) ----
    // 推导：Λ'(X_i^-1) = X_i·Π_{j≠i}(1 + X_j/X_i)，而 fcr = 0（QR 的生成多项式
    // 根从 α^0 起）时规范公式 e_i = X_i·Ω/Λ' 恰好把 X_i 约掉，所以这里**不能**
    // 再乘 X_i —— 多乘一个 X_i 会得到"位置对、值错"的假纠正，并且能被最后的
    // 伴随式复核抓到。
    for (int k = 0; k < positions.size(); ++k) {
        const quint8 xPower = powers[k];
        const quint8 xInverse = qrgalois::inverse(xPower);

        quint8 denominator = 1;
        for (int j = 0; j < positions.size(); ++j) {
            if (j == k)
                continue;
            denominator = qrgalois::multiply(
                denominator,
                static_cast<quint8>(1 ^ qrgalois::multiply(powers[j], xInverse)));
        }
        if (denominator == 0)
            return false;

        quint8 omegaValue = 0;
        for (int j = ecCount - 1; j >= 0; --j)
            omegaValue = static_cast<quint8>(
                qrgalois::multiply(omegaValue, xInverse) ^ omega[j]);

        const quint8 magnitude = qrgalois::divide(omegaValue, denominator);
        block[positions[k]] = static_cast<quint8>(block[positions[k]] ^ magnitude);
    }

    // 改完必须复核：超纠错能力的块会出现"位置找到了、值算错了"，
    // 不复核就会把乱码当成正常内容显示出来。
    if (!syndromesAreZero(nullptr))
        return false;

    corrected = positions.size();
    return true;
}

// ---------------------------------------------------------------------------
// 位流解析
// ---------------------------------------------------------------------------

/// 位读取器。二维码的位流是高位优先的，读越界即失败（不补零，
/// 补零会把损坏的码字解析成看似合法的内容）。
struct BitReader
{
    const QVector<quint8> &bytes;
    int totalBits = 0;
    int position = 0;

    bool read(int count, int &value)
    {
        if (count < 0 || position + count > totalBits)
            return false;
        value = 0;
        for (int i = 0; i < count; ++i) {
            const int bit = (bytes[(position + i) >> 3] >> (7 - ((position + i) & 7))) & 1;
            value = (value << 1) | bit;
        }
        position += count;
        return true;
    }

    int remaining() const { return totalBits - position; }
};

/// 严格校验 UTF-8（QString::fromUtf8 遇到坏字节会静默替换成 U+FFFD，
/// 那样就没法区分"本来就是乱码"和"编码判断错了"，所以要先自己验一遍）。
bool isValidUtf8(const QByteArray &bytes)
{
    int i = 0;
    const int n = bytes.size();
    while (i < n) {
        const quint8 c = static_cast<quint8>(bytes.at(i));
        if (c < 0x80) {
            ++i;
            continue;
        }

        int extra = 0;
        quint32 codePoint = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            codePoint = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            codePoint = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            codePoint = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= n)
            return false;
        for (int k = 1; k <= extra; ++k) {
            const quint8 cc = static_cast<quint8>(bytes.at(i + k));
            if ((cc & 0xC0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (cc & 0x3F);
        }
        // 拒绝过长编码、代理区与超范围码点（RFC 3629）
        if ((extra == 1 && codePoint < 0x80)
            || (extra == 2 && codePoint < 0x800)
            || (extra == 3 && codePoint < 0x10000))
            return false;
        if (codePoint >= 0xD800 && codePoint <= 0xDFFF)
            return false;
        if (codePoint > 0x10FFFF)
            return false;
        i += extra + 1;
    }
    return true;
}

/// 按 ECI 把字节段转成文本。
///
/// 没有 ECI 时先按 UTF-8 试（国内绝大多数生成器直接写 UTF-8）；
/// 不是合法 UTF-8 再试 GB18030（Qt 构建里带了该转换器时才可用），
/// 最后退回 Latin-1 —— 至少能把 ASCII 部分正确显示出来。
QString decodeByteSegment(const QByteArray &bytes, int eci)
{
    if (eci == 26 || eci == -1) {
        if (isValidUtf8(bytes))
            return QString::fromUtf8(bytes);

        QStringDecoder gb18030("GB18030");
        if (gb18030.isValid()) {
            const QString text = gb18030.decode(bytes);
            if (!text.contains(QChar::ReplacementCharacter))
                return text;
        }
        return QString::fromLatin1(bytes);
    }
    if (eci == 3)
        return QString::fromLatin1(bytes);
    // 其它 ECI（Shift-JIS / Big5 / UTF-16 等）没有内置转换器，按 UTF-8 尽力而为
    return QString::fromUtf8(bytes);
}

/// 解析数据码字里的段。返回 false 时 error 说明哪一段不支持。
bool parsePayload(const QVector<quint8> &data, int version, QString &text, QString &error)
{
    BitReader reader{data, static_cast<int>(data.size()) * 8, 0};

    const int numericCountBits = version <= 9 ? 10 : (version <= 26 ? 12 : 14);
    const int alnumCountBits = version <= 9 ? 9 : (version <= 26 ? 11 : 13);
    const int byteCountBits = version <= 9 ? 8 : 16;

    static const char kAlnumTable[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

    QString result;
    QByteArray byteBuffer;
    int currentEci = -1;

    // 把累积的字节段按当前 ECI 解码后并入结果
    const auto flushBytes = [&]() {
        if (byteBuffer.isEmpty())
            return;
        result += decodeByteSegment(byteBuffer, currentEci);
        byteBuffer.clear();
    };

    while (reader.remaining() >= 4) {
        int mode = 0;
        if (!reader.read(4, mode))
            break;
        if (mode == 0)
            break;   // 终止符

        switch (mode) {
        case 0x1: {   // 数字模式：每 3 位十进制数用 10 bit，余数用 7/4 bit
            flushBytes();
            int count = 0;
            if (!reader.read(numericCountBits, count))
                { error = QStringLiteral("数字段长度字段不完整"); return false; }
            for (int i = 0; i + 3 <= count; i += 3) {
                int value = 0;
                if (!reader.read(10, value) || value > 999)
                    { error = QStringLiteral("数字段数据损坏"); return false; }
                result += QStringLiteral("%1").arg(value, 3, 10, QLatin1Char('0'));
            }
            const int remainder = count % 3;
            if (remainder == 2) {
                int value = 0;
                if (!reader.read(7, value) || value > 99)
                    { error = QStringLiteral("数字段数据损坏"); return false; }
                result += QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
            } else if (remainder == 1) {
                int value = 0;
                if (!reader.read(4, value) || value > 9)
                    { error = QStringLiteral("数字段数据损坏"); return false; }
                result += QString::number(value);
            }
            break;
        }
        case 0x2: {   // 字母数字模式：每 2 个字符 11 bit（前一个 *45 + 后一个）
            flushBytes();
            int count = 0;
            if (!reader.read(alnumCountBits, count))
                { error = QStringLiteral("字符段长度字段不完整"); return false; }
            for (int i = 0; i + 2 <= count; i += 2) {
                int value = 0;
                if (!reader.read(11, value) || value >= 45 * 45)
                    { error = QStringLiteral("字符段数据损坏"); return false; }
                result += QLatin1Char(kAlnumTable[value / 45]);
                result += QLatin1Char(kAlnumTable[value % 45]);
            }
            if (count % 2 == 1) {
                int value = 0;
                if (!reader.read(6, value) || value >= 45)
                    { error = QStringLiteral("字符段数据损坏"); return false; }
                result += QLatin1Char(kAlnumTable[value]);
            }
            break;
        }
        case 0x4: {   // 字节模式：先攒着，遇到 ECI 或结束时统一按编码解
            int count = 0;
            if (!reader.read(byteCountBits, count))
                { error = QStringLiteral("字节段长度字段不完整"); return false; }
            if (reader.remaining() < count * 8)
                { error = QStringLiteral("字节段长度超出数据范围"); return false; }
            byteBuffer.reserve(byteBuffer.size() + count);
            for (int i = 0; i < count; ++i) {
                int value = 0;
                reader.read(8, value);
                byteBuffer.append(static_cast<char>(value));
            }
            break;
        }
        case 0x7: {   // ECI：切换后续字节段的编码
            flushBytes();
            int first = 0;
            if (!reader.read(8, first))
                { error = QStringLiteral("ECI 字段不完整"); return false; }
            if ((first & 0x80) == 0) {
                currentEci = first;
            } else if ((first & 0xC0) == 0x80) {
                int second = 0;
                if (!reader.read(8, second))
                    { error = QStringLiteral("ECI 字段不完整"); return false; }
                currentEci = ((first & 0x3F) << 8) | second;
            } else if ((first & 0xE0) == 0xC0) {
                int rest = 0;
                if (!reader.read(16, rest))
                    { error = QStringLiteral("ECI 字段不完整"); return false; }
                currentEci = ((first & 0x1F) << 16) | rest;
            } else {
                error = QStringLiteral("ECI 字段格式非法");
                return false;
            }
            break;
        }
        case 0x3: {   // 结构化拼接头：4 bit 序号 + 4 bit 总数 + 8 bit 校验，跳过即可
            flushBytes();
            int ignored = 0;
            if (!reader.read(16, ignored))
                { error = QStringLiteral("结构化拼接头不完整"); return false; }
            break;
        }
        case 0x5:
        case 0x9:
            // FNC1（GS1 / AIM）：只影响内容语义，这里不做特殊标记
            flushBytes();
            break;
        case 0x8:
            error = QStringLiteral("内容包含日文 Kanji 模式（Shift-JIS），暂不支持");
            return false;
        default:
            error = QStringLiteral("遇到无法识别的段类型 0x%1").arg(mode, 0, 16);
            return false;
        }
    }

    flushBytes();
    text = result;
    return true;
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

struct Geometry
{
    FinderPoint tl;
    FinderPoint tr;
    FinderPoint bl;
    int dimension = 0;
};

/// 按模块中心采样，得到 dimension×dimension 的黑白矩阵。
QVector<quint8> sampleGrid(const QVector<quint8> &bin, int w, int h,
                           const ProjectiveMap &map, int dimension)
{
    QVector<quint8> modules(dimension * dimension, 0);
    for (int y = 0; y < dimension; ++y) {
        for (int x = 0; x < dimension; ++x) {
            const QPointF point = map.map(x + 0.5, y + 0.5);
            if (point.x() < 0 || point.y() < 0)
                continue;
            const int px = qBound(0, int(std::floor(point.x())), w - 1);
            const int py = qBound(0, int(std::floor(point.y())), h - 1);
            modules[y * dimension + x] = bin[py * w + px];
        }
    }
    return modules;
}

/// 读格式信息：两份副本各自与 32 个合法码字比对，取汉明距离最小的一份。
bool readFormatInfo(const QVector<quint8> &modules, int dimension, int &eccBits, int &mask)
{
    const auto bit = [&](int x, int y) -> int { return modules[y * dimension + x]; };

    int copy1 = 0;
    for (int i = 0; i <= 5; ++i)
        copy1 |= bit(8, i) << i;
    copy1 |= bit(8, 7) << 6;
    copy1 |= bit(8, 8) << 7;
    copy1 |= bit(7, 8) << 8;
    for (int i = 9; i < 15; ++i)
        copy1 |= bit(14 - i, 8) << i;

    int copy2 = 0;
    for (int i = 0; i < 8; ++i)
        copy2 |= bit(dimension - 1 - i, 8) << i;
    for (int i = 8; i < 15; ++i)
        copy2 |= bit(8, dimension - 15 + i) << i;

    int bestData = -1;
    int bestDistance = 99;
    for (int data = 0; data < 32; ++data) {
        const int candidate = formatCode(data >> 3, data & 0x7);
        const int distance = qMin(qPopulationCount(quint32(candidate ^ copy1)),
                                  qPopulationCount(quint32(candidate ^ copy2)));
        if (distance < bestDistance) {
            bestDistance = distance;
            bestData = data;
        }
    }
    if (bestData < 0 || bestDistance > kMaxFormatErrors)
        return false;

    eccBits = bestData >> 3;
    mask = bestData & 0x7;
    return true;
}

/// 取码字（反掩码 + 蛇形顺序），再反交织、逐块纠错，最后拼回数据码字。
bool extractData(const QVector<quint8> &modules, int version, Ecc ecc, int mask,
                 QVector<quint8> &dataCodewords, int &corrected, QString &error)
{
    const int dimension = sizeForVersion(version);
    const QVector<quint8> reserved = reservedMap(version);
    const int totalBits = totalCodewords(version) * 8;

    QVector<quint8> bits;
    bits.reserve(totalBits);
    forEachDataModule(dimension, reserved, [&](int x, int y, int) {
        quint8 bit = modules[y * dimension + x];
        if (isMasked(mask, x, y))
            bit = static_cast<quint8>(bit ^ 1);
        bits.append(bit);
        return bits.size() < totalBits;
    });
    if (bits.size() < totalBits) {
        error = QStringLiteral("数据区不完整（版本或定位有误）");
        return false;
    }

    QVector<quint8> codewords(totalBits / 8, 0);
    for (int i = 0; i < totalBits; ++i) {
        if (bits[i])
            codewords[i >> 3] = static_cast<quint8>(codewords[i >> 3] | (0x80 >> (i & 7)));
    }

    // ---- 反交织：按"每块轮流取一个"的逆过程还原 ----
    const EccBlockSpec &spec = blocksFor(version, ecc);
    struct Block
    {
        int dataLength = 0;
        QVector<quint8> bytes;
    };
    QVector<Block> blocks;
    blocks.reserve(int(spec.blocks1) + int(spec.blocks2));
    const auto appendBlocks = [&](int count, int dataLength) {
        for (int i = 0; i < count; ++i) {
            Block block;
            block.dataLength = dataLength;
            blocks.append(block);
        }
    };
    appendBlocks(spec.blocks1, spec.data1);
    appendBlocks(spec.blocks2, spec.data2);

    int cursor = 0;
    int maxDataLength = 0;
    for (const Block &block : blocks)
        maxDataLength = qMax(maxDataLength, block.dataLength);

    for (int i = 0; i < maxDataLength; ++i) {
        for (Block &block : blocks) {
            if (i < block.dataLength)
                block.bytes.append(codewords[cursor++]);
        }
    }
    for (int i = 0; i < spec.ecPerBlock; ++i) {
        for (Block &block : blocks)
            block.bytes.append(codewords[cursor++]);
    }

    dataCodewords.clear();
    corrected = 0;
    for (Block &block : blocks) {
        int blockCorrected = 0;
        if (!reedSolomonDecode(block.bytes, spec.ecPerBlock, blockCorrected)) {
            error = QStringLiteral("纠错失败：损坏超出容错等级 %1 能恢复的范围")
                        .arg(eccName(ecc));
            return false;
        }
        corrected += blockCorrected;
        dataCodewords += block.bytes.mid(0, block.dataLength);
    }
    return true;
}

/// 对一个候选版本做完整解码。成功时填好 result 并返回 true。
bool decodeAtVersion(const QVector<quint8> &bin, int w, int h,
                     const FinderPoint &tl, const FinderPoint &tr, const FinderPoint &bl,
                     int version, DecodeResult &result, QString &error)
{
    const int dimension = sizeForVersion(version);
    const double moduleSize = (tl.module + tr.module + bl.module) / 3.0;

    ProjectiveMap map = affineMap(tl, tr, bl, dimension);

    // 校正图形把右下角也钉住，透视/镜头畸变才不会被放大到整个下半区
    int alignCount = 0;
    alignmentPositions(version, alignCount);
    if (alignCount > 0) {
        const QPointF predicted = map.map(dimension - 6.5, dimension - 6.5);
        QPointF alignmentCenter;
        if (findAlignment(bin, w, h, predicted, moduleSize, alignmentCenter)) {
            const QPointF src[4] = {
                QPointF(3.5, 3.5),
                QPointF(dimension - 3.5, 3.5),
                QPointF(3.5, dimension - 3.5),
                QPointF(dimension - 6.5, dimension - 6.5),
            };
            const QPointF dst[4] = {
                QPointF(tl.x, tl.y),
                QPointF(tr.x, tr.y),
                QPointF(bl.x, bl.y),
                alignmentCenter,
            };
            ProjectiveMap projective;
            if (solveHomography(src, dst, projective))
                map = projective;
        }
    }

    const QVector<quint8> modules = sampleGrid(bin, w, h, map, dimension);

    int eccBits = 0;
    int mask = 0;
    if (!readFormatInfo(modules, dimension, eccBits, mask)) {
        error = QStringLiteral("格式信息读取失败（可能被遮挡或图片损坏）");
        return false;
    }

    Ecc ecc = Ecc::Medium;
    if (!eccFromFormatBits(eccBits, ecc)) {
        error = QStringLiteral("格式信息里的容错等级非法");
        return false;
    }

    QVector<quint8> dataCodewords;
    int corrected = 0;
    QString extractError;
    if (!extractData(modules, version, ecc, mask, dataCodewords, corrected, extractError)) {
        error = extractError;
        return false;
    }

    QString text;
    QString parseError;
    if (!parsePayload(dataCodewords, version, text, parseError)) {
        error = parseError;
        return false;
    }

    result.ok = true;
    result.text = text;
    result.version = version;
    result.mask = mask;
    result.eccName = eccName(ecc);
    result.correctedCodewords = corrected;
    return true;
}

/// 在一种二值化结果上跑完整流程。失败时 why 里是最后一处失败原因。
bool decodeBinary(const QVector<quint8> &bin, int w, int h, DecodeResult &result, QString &why)
{
    const QVector<FinderPoint> candidates = findFinderPatterns(bin, w, h);
    if (candidates.size() < 3) {
        why = QStringLiteral("没找到二维码的定位图形（图片里可能没有二维码，或对比度太低）");
        return false;
    }

    const QVector<FinderTriple> triples = rankTriples(candidates);
    if (triples.isEmpty()) {
        why = QStringLiteral("定位图形的位置不成直角，可能不是完整的二维码");
        return false;
    }

    // 组合数可能上百（数据区的同构片段很多），逐个"试解"。每试一个都要采样
    // 整张矩阵，所以设一个硬上限：正常图片第一个组合就成功，脏图也不至于卡住。
    constexpr int kMaxTriples = 24;
    constexpr int kMaxAttempts = 40;
    const int offsets[5] = {0, 1, -1, 2, -2};

    int triplesTried = 0;
    int attempts = 0;
    QString lastError;
    for (const FinderTriple &triple : triples) {
        if (triplesTried >= kMaxTriples || attempts >= kMaxAttempts)
            break;
        ++triplesTried;

        // 用两个定位图形的平均距离估版本，再在附近 ±2 内重试：
        // 模块尺寸的估计误差 ±1 版很常见，只试一个值会莫名其妙地失败。
        const double legTR = distanceBetween(triple.tl, triple.tr);
        const double legBL = distanceBetween(triple.tl, triple.bl);
        const double moduleSize =
            (triple.tl.module + triple.tr.module + triple.bl.module) / 3.0;
        const double estimated = (legTR + legBL) / 2.0 / qMax(0.5, moduleSize) + 7.0;
        const int estimatedVersion =
            qBound(1, int(std::lround((estimated - 17.0) / 4.0)), 40);

        for (const int offset : offsets) {
            if (attempts >= kMaxAttempts)
                break;
            const int version = estimatedVersion + offset;
            if (version < 1 || version > 40)
                continue;
            ++attempts;
            QString error;
            if (decodeAtVersion(bin, w, h, triple.tl, triple.tr, triple.bl,
                                version, result, error)) {
                return true;
            }
            lastError = error;
        }
    }

    why = lastError;
    return false;
}

} // namespace

DecodeResult decodeImage(const QImage &image)
{
    DecodeResult result;
    if (image.isNull()) {
        result.error = QStringLiteral("图片为空");
        return result;
    }

    const GrayImage gray = toGray(image);
    if (gray.width < 21 || gray.height < 21) {
        result.error = QStringLiteral("图片太小，至少需要 21×21 像素");
        return result;
    }

    // 先全局阈值（截图/导出图一次就过）；失败再试局部归一化（照片光照不均）。
    // 局部二值化要建积分图，放在第二次才算是刻意的：绝大多数输入走不到这里。
    QString firstError;
    const QVector<quint8> global = binarizeOtsu(gray);
    if (decodeBinary(global, gray.width, gray.height, result, firstError))
        return result;

    const QVector<quint8> local = binarizeNormalized(gray);
    QString secondError;
    if (decodeBinary(local, gray.width, gray.height, result, secondError))
        return result;

    // 两次失败原因取后者：局部阈值那次通常更接近真实情况（照片）
    result.ok = false;
    result.error = secondError.isEmpty() ? firstError : secondError;
    return result;
}

} // namespace qrcode

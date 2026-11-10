#pragma once

// ---------------------------------------------------------------------------
// OCR 纯逻辑：PP-OCR（DBNet 检测 + CRNN/SVTR 识别）的后处理。
//
// 为什么单独抽成 header（与 OcrText.h 同理由）：
//   * 规则 2 要求改动必须有实际跑过的验证，纯逻辑放进 header 才能在临时工程里
//     直接 include 做断言，不必把 onnxruntime 拖进测试；
//   * det 的概率图阈值化/连通域/外接矩形、rec 的 CTC 解码都是纯数学运算，
//     与推理引擎无关，抽出来既可单测，也保证 onnxruntime 头文件不被扩散到别处。
//
// 约束：只依赖 QtCore 的值类型（QVector/QString/QStringList），不 include 任何
// onnxruntime / QWidget 头。所有函数都是纯函数，输入输出可完全预测。
//
// 注意：这些函数在头文件里直接写实现，且会被多个翻译单元（OcrEngine.cpp /
// OcrOnnx.cpp）同时 include，所以必须标 inline——否则每个 .obj 各导出一份符号，
// 链接期报 LNK2005 多重定义。
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>

namespace ocrpost {

/// 检测出的一个文字框（轴对齐外接矩形）。PP-OCR det 输出的是四边形，
/// 这里取 min/max 归一化成矩形 —— 截图场景文字基本水平，够用；
/// 真斜排文字（需要旋转矩形）留作后续扩展点。
struct DetBox
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;   ///< 右边界（不含），保证 width = x1 - x0 恒正
    int y1 = 0;   ///< 下边界（不含）
    float score = 0.0f;

    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};

/// 概率图 → 文字框列表。
///
/// 严格照 PaddleOCR 官方 DB 后处理步骤：后处理阈值化 → 连通域 → 外接矩形 →
/// **unclip 膨胀**。unclip 是必须的：DB-Net 输出的概率图是「文字收缩后的核心区」，
/// 不是完整文字框（实测收缩框只有文字真实高度的约 40%），不膨胀会把文字上下
/// 边缘切掉，rec 识别直接崩坏。膨胀量按 PaddleOCR 公式 offset = area × ratio / perimeter。
///
/// @param probMap 概率图，行主序，尺寸 mapW × mapH，值域 [0,1]（sigmoid 输出）
/// @param mapW/mapH 概率图的宽高（det 输出尺寸，等于输入 pad 后的尺寸）
/// @param origW/origH 缩放图的宽高（det 输入 pad 前的尺寸），仅用于把框坐标夹紧
///                    （概率图文字区域与该尺寸 1:1 对齐，不参与缩放）
/// @param threshold 二值化阈值，PaddleOCR 默认 0.3
/// @param unclipRatio 膨胀倍率，PaddleOCR 默认 2.0
/// @param minSize 过滤掉宽或高小于该值（概率图空间）的噪声框
/// @return 按「先 y 后 x」阅读顺序排好的框；概率图无效时返回空
inline QVector<DetBox> detBoxes(const QVector<float> &probMap,
                         int mapW, int mapH,
                         int origW, int origH,
                         float threshold = 0.3f, float unclipRatio = 2.0f,
                         int minSize = 3)
{
    QVector<DetBox> boxes;
    if (probMap.size() < mapW * mapH || mapW <= 0 || mapH <= 0)
        return boxes;

    // 连通域标记：visited 记录每个像素是否已归属某个连通域。
    // 用广度优先（队列）而非递归，避免深图栈溢出。
    QVector<quint8> visited(mapW * mapH, 0);

    const auto idx = [mapW](int x, int y) { return y * mapW + x; };

    for (int sy = 0; sy < mapH; ++sy) {
        for (int sx = 0; sx < mapW; ++sx) {
            const int startIdx = idx(sx, sy);
            // 只从「未访问且超过阈值」的像素开始一次新连通域
            if (visited[startIdx] || probMap[startIdx] < threshold)
                continue;

            // BFS 找整块连通域，同时累计外接矩形边界与平均置信度
            QVector<int> stack;
            stack.reserve(64);
            stack.append(startIdx);
            visited[startIdx] = 1;

            int minX = sx, maxX = sx, minY = sy, maxY = sy;
            float scoreSum = 0.0f;
            int count = 0;

            while (!stack.isEmpty()) {
                const int cur = stack.takeLast();
                const int cx = cur % mapW;
                const int cy = cur / mapW;
                minX = qMin(minX, cx); maxX = qMax(maxX, cx);
                minY = qMin(minY, cy); maxY = qMax(maxY, cy);
                scoreSum += probMap[cur];
                ++count;

                // 8 邻域（上下左右 + 4 个对角）。为什么用 8 邻域而非 4 邻域：
                // 中文字笔画常有对角相接（如「体」「衡」的偏旁），4 邻域会把
                // 对角断开的笔画切成多个连通域，导致 det 框不完整、crop 时切掉
                // 半个字，rec 识别出「休」「绝」这类错字。scipy 的 ndimage.label
                // 默认就是 8 邻域，PaddleOCR 官方后处理也用它（实测 8 邻域结果
                // 与官方完全一致，4 邻域则多出 2-3 个错字）。
                const int nx[8] = { cx - 1, cx + 1, cx,     cx,
                                    cx - 1, cx - 1, cx + 1, cx + 1 };
                const int ny[8] = { cy,     cy,     cy - 1, cy + 1,
                                    cy - 1, cy + 1, cy - 1, cy + 1 };
                for (int k = 0; k < 8; ++k) {
                    if (nx[k] < 0 || nx[k] >= mapW || ny[k] < 0 || ny[k] >= mapH)
                        continue;
                    const int nIdx = idx(nx[k], ny[k]);
                    if (!visited[nIdx] && probMap[nIdx] >= threshold) {
                        visited[nIdx] = 1;
                        stack.append(nIdx);
                    }
                }
            }

            // 过滤噪点：过小框（宽或高不足 minSize）视为误检
            const int boxW = maxX - minX + 1;
            const int boxH = maxY - minY + 1;
            if (boxW < minSize || boxH < minSize)
                continue;

            // unclip 膨胀：offset = area * ratio / perimeter。矩形近似（截图场景
            // 文字基本水平），把收缩框向四周各扩 offset。PaddleOCR 用 Vatti clipping
            // 精确膨胀，但对水平文字矩形近似误差可忽略，且避免引入 Clipper 依赖。
            const float area = float(boxW) * float(boxH);
            const float perimeter = 2.0f * (float(boxW) + float(boxH));
            const float offset = perimeter > 0.0f
                ? area * unclipRatio / perimeter : 0.0f;

            DetBox box;
            box.score = count > 0 ? scoreSum / float(count) : 0.0f;
            // 坐标映射：概率图尺寸 == det 输入 pad 后尺寸，而 pad 只在右侧/下方
            // 加白边，文字区域与「pad 前的缩放图」是 1:1 对齐的。所以这里**不做
            // 缩放**（sx_=sy_=1.0），直接取概率图坐标，再夹紧到缩放图尺寸即可。
            // 若按 origW/mapW 缩放会把框整体缩小，实测会切掉文字上下边缘（识别
            // 出「休」「绝」这类错字）。origW/origH 只用于 clamp。
            box.x0 = int(float(minX) - offset);
            box.y0 = int(float(minY) - offset);
            box.x1 = int(float(maxX + 1) + offset);
            box.y1 = int(float(maxY + 1) + offset);
            // 边界夹紧到缩放图尺寸（文字不会跑到 pad 白边之外）
            box.x0 = qBound(0, box.x0, origW);
            box.y0 = qBound(0, box.y0, origH);
            box.x1 = qBound(box.x0, box.x1, origW);
            box.y1 = qBound(box.y0, box.y1, origH);
            boxes.append(box);
        }
    }

    // 阅读顺序：先按 y 排序，同行（y 接近）再按 x。
    // 这里只做简单排序，行内归并交给上层 OcrText.h 的 groupLines（它已有
    // 按 y 容差聚行 + 行内按 x 排序的成熟逻辑），此处不必重复实现。
    std::sort(boxes.begin(), boxes.end(), [](const DetBox &a, const DetBox &b) {
        if (a.y0 != b.y0)
            return a.y0 < b.y0;
        return a.x0 < b.x0;
    });
    return boxes;
}

/// CTC 概率序列 → 文本。
///
/// 标准 CTC greedy 解码（PaddleOCR CTCLabelDecode 的 is_remove_duplicate=True 分支）：
///   逐时间步 argmax → 去连续重复 → 去 blank（索引 0）→ 按 keys 映射字符。
/// 顺序必须是「先去连续重复、再去 blank」，不能反过来：序列 A A - B B C 里
/// 若先删 blank 会得到 A A B B C，再合并成 A B C 虽然碰巧对，但遇到 A - A 会
/// 错成 AA 而非 A。PaddleOCR 官方即先去重后去 blank。
///
/// @param probs 识别输出，布局为 timesteps × vocabSize（行主序），已过 softmax
/// @param timesteps 时间步数 T
/// @param vocabSize 词表大小（= keys.size() + 2：blank + keys 字符 + 对齐空格）
/// @param keys 字符表（ppocr_keys_v1.txt 的每一行，第 i 项对应类索引 i+1）
/// @return 解码出的文本；空串表示全是 blank/无效
inline QString ctcDecode(const QVector<float> &probs, int timesteps, int vocabSize,
                  const QStringList &keys)
{
    QString out;
    if (timesteps <= 0 || vocabSize <= 0 || probs.size() < timesteps * vocabSize)
        return out;

    int lastClass = -1;   // 上一个非 blank 的类索引（用于去连续重复）
    for (int t = 0; t < timesteps; ++t) {
        // 逐帧 argmax
        const float *row = probs.constData() + size_t(t) * size_t(vocabSize);
        int best = 0;
        float bestVal = row[0];
        for (int c = 1; c < vocabSize; ++c) {
            if (row[c] > bestVal) {
                bestVal = row[c];
                best = c;
            }
        }
        if (best == 0) {          // blank：跳过，但把「连续重复」状态重置
            lastClass = -1;
            continue;
        }
        if (best == lastClass)    // 与上一帧相同的字符（CTC 的重复帧）→ 合并
            continue;
        lastClass = best;
        // 类索引 best（1..keys.size()+1）→ 字符：best-1 是 keys 下标，
        // best == keys.size()+1 对应官方额外追加的空格字符
        if (best <= keys.size()) {
            out += keys.at(best - 1);
        } else {
            out += QLatin1Char(' ');   // 空格类（keys 文件里没有，官方 append 的）
        }
    }
    return out;
}

} // namespace ocrpost

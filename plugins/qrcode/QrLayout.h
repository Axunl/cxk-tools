#pragma once

// ---------------------------------------------------------------------------
// 二维码矩阵的"画布"规则：功能图形、格式信息、版本信息、掩码、数据填充顺序。
//
// 编码与解码必须对"哪些格子是功能模块"有完全一致的认知。这里把这份规则收成
// 一份实现，两边共用：编码器照它画图案、放数据，解码器照它跳过功能模块、读数据。
// 如果两边各写一份，某个版本（例如 v7 才开始有版本信息区）出问题时极难定位。
//
// 坐标系约定：x = 列，y = 行，原点在左上角；矩阵按行优先存成 size*size 的一维数组，
// 下标 = y * size + x。这与 QImage 的像素扫描顺序一致，最后渲染时不用转置。
// ---------------------------------------------------------------------------

#include "QrTables.h"

#include <QVector>
#include <QtGlobal>

namespace qrcode {

////
/// 格式信息的 15 位编码：2 位容错等级 + 3 位掩码 + 10 位 BCH 校验，再异或固定掩码。
///
/// BCH 生成多项式 0x537、固定掩码 0x5412 都是规范写死的值，不是可调参数。
/// 解码时按同样的公式生成 32 个合法码字逐一比对（见 QrDecode.cpp），
/// 所以这两处必须用同一套常数。
///
inline int formatCode(int eccFormat, int mask)
{
    const int data = ((eccFormat & 0x3) << 3) | (mask & 0x7);
    int rem = data;
    for (int i = 0; i < 10; ++i)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return ((data << 10) | (rem & 0x3FF)) ^ 0x5412;
}

/// 版本信息的 18 位编码（6 位版本号 + 12 位 BCH，生成多项式 0x1F25）。
inline int versionCode(int version)
{
    int rem = version;
    for (int i = 0; i < 12; ++i)
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    return (version << 12) | (rem & 0xFFF);
}

////
/// 把 15 位格式信息写到两处（左上角一圈、右上角 + 左下角各半），并标记为功能模块。
/// 两份内容完全相同，扫码器只要有一份完整就能读出来 —— 这也是为什么贴纸遮住
/// 一角时它还能工作。
///
inline void placeFormat(QVector<quint8> &modules, QVector<quint8> &reserved, int size, int bits)
{
    const auto set = [&](int x, int y, int index) {
        const quint8 value = static_cast<quint8>((bits >> index) & 1);
        modules[y * size + x] = value;
        reserved[y * size + x] = 1;
    };

    // 第 1 份：左上角，绕定位图形一圈
    for (int i = 0; i <= 5; ++i)
        set(8, i, i);
    set(8, 7, 6);
    set(8, 8, 7);
    set(7, 8, 8);
    for (int i = 9; i < 15; ++i)
        set(14 - i, 8, i);

    // 第 2 份：右上角（bit 0–7）+ 左下角（bit 8–14）
    for (int i = 0; i < 8; ++i)
        set(size - 1 - i, 8, i);
    for (int i = 8; i < 15; ++i)
        set(8, size - 15 + i, i);
}

////
/// 画全部功能图形，并把它们占用的格子标记为"功能模块"（不承载数据）。
///
/// 顺序有讲究：
///   1. 定位线先画，随后定位图形会覆盖它两端 —— 反过来画的话，
///      定位线会把定位图形中间那一行/列戳成黑白相间，扫码器直接找不到定位点；
///   2. 校正图形在定位线之后画：规范里它们的 5×5 范围就是会盖住定位线；
///   3. 格式信息先按"掩码 0、容错 M"写成占位值，只为把格子占住，
///      真正的比特由 placeFormat() 在选定掩码后重写。
///
/// @param version  1–40
/// @param modules  输出：size*size，1 = 深色（按 version 自行分配尺寸）
/// @param reserved 输出：size*size，1 = 功能模块
///
inline void drawBasePatterns(int version, QVector<quint8> &modules, QVector<quint8> &reserved)
{
    const int size = sizeForVersion(version);
    modules.assign(size * size, 0);
    reserved.assign(size * size, 0);

    // setFunction：写颜色并标记占用。"功能模块"这个标记是数据填充时的唯一依据，
    // 忘了标就会出现数据把定位图形覆盖掉、但矩阵看上去仍然"有图案"的怪状。
    const auto setFunction = [&](int x, int y, bool dark) {
        if (x < 0 || y < 0 || x >= size || y >= size)
            return;
        modules[y * size + x] = dark ? 1 : 0;
        reserved[y * size + x] = 1;
    };

    // ---- 定位线（第 6 行 / 第 6 列，深浅交替）----
    for (int i = 0; i < size; ++i) {
        setFunction(6, i, i % 2 == 0);
        setFunction(i, 6, i % 2 == 0);
    }

    // ---- 三个定位图形 + 分隔符（半径 4 的方框，最外圈是白色分隔带）----
    const auto drawFinder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int dist = qMax(qAbs(dx), qAbs(dy));
                // 3×3 实心(dist<=1) → 白环(2) → 黑框(3) → 白分隔带(4)
                setFunction(cx + dx, cy + dy, dist != 2 && dist != 4);
            }
        }
    };
    drawFinder(3, 3);
    drawFinder(size - 4, 3);
    drawFinder(3, size - 4);

    // ---- 校正图形（5×5：实心中心 + 白环 + 黑框）----
    int alignCount = 0;
    const int *align = alignmentPositions(version, alignCount);
    for (int i = 0; i < alignCount; ++i) {
        for (int j = 0; j < alignCount; ++j) {
            // 三个角上已经有定位图形，不再画校正图形
            const bool corner = (i == 0 && j == 0)
                             || (i == 0 && j == alignCount - 1)
                             || (i == alignCount - 1 && j == 0);
            if (corner)
                continue;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int dist = qMax(qAbs(dx), qAbs(dy));
                    setFunction(align[i] + dx, align[j] + dy, dist != 1);
                }
            }
        }
    }

    // ---- 先用占位值占住格式信息区，选定掩码后再重写 ----
    placeFormat(modules, reserved, size, formatCode(eccFormatBits(Ecc::Medium), 0));

    // ---- 版本信息区（v7 起才有，18 个比特，在两处各画一遍）----
    if (version >= 7) {
        const int bits = versionCode(version);
        for (int i = 0; i < 18; ++i) {
            const bool bit = ((bits >> i) & 1) != 0;
            const int a = size - 11 + i % 3;
            const int b = i / 3;
            setFunction(a, b, bit);
            setFunction(b, a, bit);
        }
    }

    // ---- 固定的暗模块（格式信息区旁的那一个黑点）----
    setFunction(8, size - 8, true);
}

///
/// 只求功能模块占用图。解码器用它跳过功能模块；内部就是画一遍占位图案，
/// 保证与编码器完全一致（图案颜色无所谓，只读 reserved）。
///
inline QVector<quint8> reservedMap(int version)
{
    QVector<quint8> modules;
    QVector<quint8> reserved;
    drawBasePatterns(version, modules, reserved);
    return reserved;
}

////
/// 掩码条件。掩码的作用是把大片同色区域打散，让扫码器更容易分辨模块边界；
/// 8 种都是规范定义的固定公式，不能自创（格式信息里只存 3 位掩码编号）。
///
inline bool isMasked(int mask, int x, int y)
{
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return (x * y) % 2 + (x * y) % 3 == 0;
    case 6: return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
    case 7: return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    }
    return false;
}

////
/// 数据比特的填充顺序：从右下角开始，两列一组向上/向下蛇形前进，跳过第 6 列
/// （整列是定位线）。这是规范定义的顺序，编码与解码各写反一次就会得到"能扫出
/// 内容但全是乱码"的结果，所以两边都用这个函数。
///
/// @param visit 回调 (x, y, bitIndex)，bitIndex 从 0 递增；返回 false 可提前结束
///
template <typename Visitor>
inline void forEachDataModule(int size, const QVector<quint8> &reserved, Visitor visit)
{
    int bitIndex = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6)
            right = 5;   // 跳过整列定位线
        for (int vert = 0; vert < size; ++vert) {
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                // (right+1)&2 与 right 的奇偶同步：同一对列走完再反向
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size - 1 - vert : vert;
                if (reserved[y * size + x])
                    continue;
                if (!visit(x, y, bitIndex))
                    return;
                ++bitIndex;
            }
        }
    }
}

} // namespace qrcode

#pragma once

// ---------------------------------------------------------------------------
// GF(256) 有限域运算 + Reed-Solomon 系统码编码。
//
// 二维码的纠错码字全部在 GF(2^8) 上计算，本原多项式固定为 0x11D
// （x^8 + x^4 + x^3 + x^2 + 1）。这是 QR 规范指定的那一个：换成别的本原多项式
// （如 0x187）算出来的结果看起来同样是"能纠错的一堆字节"，但任何扫码器都读不出来，
// 所以这个常数不是可以随便调的参数。
//
// 为什么把 exp/log 放进函数内静态对象：表只有 256 项，首次调用时建一次，
// 之后都是查表；写成全局变量会引入跨编译单元的初始化顺序问题（插件里尤其麻烦）。
// ---------------------------------------------------------------------------

#include <QVector>
#include <QtGlobal>

namespace qrgalois {

/// 乘法群阶数（非零元素个数），也是 exp 表的循环周期。
constexpr int kOrder = 255;

/// 预计算的指数 / 对数表。log[0] 无定义，这里填 0 占位（调用方必须先判 0）。
struct FieldTables
{
    quint8 exp[512];   ///< 512 项：exp[a + b] 直接取，省掉每次取模
    quint8 log[256];

    FieldTables()
    {
        int value = 1;
        for (int i = 0; i < kOrder; ++i) {
            exp[i] = static_cast<quint8>(value);
            log[value] = static_cast<quint8>(i);
            value <<= 1;
            if (value & 0x100)
                value ^= 0x11D;   // 溢出时按本原多项式约减
        }
        for (int i = kOrder; i < 512; ++i)
            exp[i] = exp[i - kOrder];
        log[0] = 0;
    }
};

/// 全进程共享一份表（静态局部变量，C++ 保证线程安全的一次性初始化）。
inline const FieldTables &field()
{
    static const FieldTables tables;
    return tables;
}

inline quint8 multiply(quint8 a, quint8 b)
{
    if (a == 0 || b == 0)
        return 0;
    const FieldTables &f = field();
    return f.exp[int(f.log[a]) + int(f.log[b])];
}

inline quint8 inverse(quint8 a)
{
    // 零元素没有逆元；调用方必须先排除（本模块内部只在已知非零处调用）
    const FieldTables &f = field();
    return f.exp[kOrder - int(f.log[a])];
}

inline quint8 divide(quint8 a, quint8 b)
{
    if (a == 0)
        return 0;
    const FieldTables &f = field();
    return f.exp[(int(f.log[a]) - int(f.log[b]) + kOrder) % kOrder];
}

/// α^e（α 是本原元 2）。e 超过 255 自动回绕。
inline quint8 expOf(int e)
{
    return field().exp[((e % kOrder) + kOrder) % kOrder];
}

/// 生成多项式 g(x) = Π(x - α^i)，i = 0 … degree-1。
///
/// 返回 **不含最高次项** 的 degree 个系数，顺序为高次 → 低次（与长除法里
/// 每次异或的那一段对齐）。最高次恒为 1，不需要存。
inline QVector<quint8> generator(int degree)
{
    // 先按低次 → 高次计算，最后翻转；这样下标 i 就是 x^i 的次数，推导直观。
    QVector<quint8> poly(1, 1);
    for (int i = 0; i < degree; ++i) {
        const quint8 root = expOf(i);
        QVector<quint8> next(poly.size() + 1, 0);
        for (int j = 0; j < poly.size(); ++j) {
            next[j + 1] ^= poly[j];                 // × x
            next[j] ^= multiply(poly[j], root);     // × α^i
        }
        poly = next;
    }

    // 去掉最高次的 1，翻转成高次→低次：poly 有 degree+1 项，下标 degree 是最高次
    QVector<quint8> result(degree, 0);
    for (int i = 0; i < degree; ++i)
        result[i] = poly[degree - 1 - i];
    return result;
}

/// 系统码余式：把 data 看作多项式乘上 x^degree 后除以生成多项式，返回余式
/// （即 degree 个纠错码字，高次→低次）。
///
/// 实现是标准的移位寄存器长除法：每读入一个数据字节，用"当前余式首字节 ⊕ 数据字节"
/// 去乘生成多项式再异或回去。degree 最大 30，代价可以忽略。
inline QVector<quint8> remainder(const QVector<quint8> &data, int degree)
{
    const QVector<quint8> gen = generator(degree);
    QVector<quint8> rem(degree, 0);

    for (const quint8 byte : data) {
        const quint8 factor = static_cast<quint8>(byte ^ rem[0]);
        rem.removeFirst();
        rem.append(0);
        if (factor == 0)
            continue;
        for (int i = 0; i < degree; ++i)
            rem[i] = static_cast<quint8>(rem[i] ^ multiply(gen[i], factor));
    }
    return rem;
}

} // namespace qrgalois

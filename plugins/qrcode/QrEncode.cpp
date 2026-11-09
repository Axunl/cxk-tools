#include "QrEncode.h"

// ---------------------------------------------------------------------------
// 编码流程（每一步都对应规范里的一节，顺序不能换）：
//   1. 选版本：找最小的、能装下 byte 模式位流的版本；
//   2. 拼位流：模式指示符 → 字符计数 → 数据 → 终止符 → 补齐字节 → 填充码字；
//   3. 分块 + RS 纠错 → 交织（数据在前、纠错在后，按块轮流取）；
//   4. 画功能图形，蛇形填入数据比特；
//   5. 8 种掩码逐个试，按规范的 4 条罚分规则选最低分；
//   6. 写格式信息（容错等级 + 掩码号），渲染成图片。
// ---------------------------------------------------------------------------

#include "QrGalois.h"
#include "QrLayout.h"

#include <QByteArray>
#include <QPainter>

namespace qrcode {

namespace {

/// 位缓冲：按"高位优先"逐位追加。二维码的所有头字段都是定长位域，
/// 用字节容器 + 位计数比 std::vector<bool> 更好对账（长度已知、能直接取第 i 位）。
struct BitBuffer
{
    QVector<quint8> bytes;
    int bitLength = 0;

    void append(int value, int bits)
    {
        for (int i = bits - 1; i >= 0; --i) {
            if ((bitLength & 7) == 0)
                bytes.append(0);
            if ((value >> i) & 1)
                bytes.last() = static_cast<quint8>(bytes.last() | (0x80 >> (bitLength & 7)));
            ++bitLength;
        }
    }
};

/// byte 模式的字符计数字段长度：v1–9 是 8 位，之后是 16 位。
int byteCountBits(int version)
{
    return version <= 9 ? 8 : 16;
}

/// 罚分规则（规范 Table 24）。总分越低越好。
///
/// 这些规则的目的都是"让扫码器更好定位"：大片同色、2×2 同色块、
/// 长得像定位图形的片段、深色比例严重偏离一半，都会降低识别率。
int penaltyScore(const QVector<quint8> &modules, int size)
{
    int score = 0;

    // 规则 1：行 / 列上连续 5 个及以上同色 → 3 + (长度 - 5)
    for (int pass = 0; pass < 2; ++pass) {
        for (int a = 0; a < size; ++a) {
            int runColor = -1;
            int runLength = 0;
            for (int b = 0; b < size; ++b) {
                const int color = pass == 0 ? modules[a * size + b] : modules[b * size + a];
                if (color == runColor) {
                    ++runLength;
                    continue;
                }
                if (runLength >= 5)
                    score += 3 + (runLength - 5);
                runColor = color;
                runLength = 1;
            }
            if (runLength >= 5)
                score += 3 + (runLength - 5);
        }
    }

    // 规则 2：2×2 同色块 → 每块 3 分
    for (int y = 0; y + 1 < size; ++y) {
        for (int x = 0; x + 1 < size; ++x) {
            const quint8 c = modules[y * size + x];
            if (c == modules[y * size + x + 1]
                && c == modules[(y + 1) * size + x]
                && c == modules[(y + 1) * size + x + 1])
                score += 3;
        }
    }

    // 规则 3：出现 1:1:3:1:1 且一侧带 4 个浅色（和定位图形撞脸）→ 每次 40 分。
    // 直接用两个 11 位窗口比对，比数游程简单且不容易在边界上数错。
    constexpr int kPatternA = 0x5D0;   // 10111010000
    constexpr int kPatternB = 0x05D;   // 00001011101
    for (int pass = 0; pass < 2; ++pass) {
        for (int a = 0; a < size; ++a) {
            for (int b = 0; b + 10 < size; ++b) {
                int bits = 0;
                for (int k = 0; k < 11; ++k) {
                    const int value = pass == 0 ? modules[a * size + b + k]
                                                : modules[(b + k) * size + a];
                    bits = (bits << 1) | value;
                }
                if (bits == kPatternA || bits == kPatternB)
                    score += 40;
            }
        }
    }

    // 规则 4：深色占比每偏离 5% → 10 分
    int dark = 0;
    for (const quint8 cell : modules)
        dark += cell;
    const int total = size * size;
    const int deviation = (qAbs(dark * 20 - total * 10) + total - 1) / total - 1;
    if (deviation > 0)
        score += deviation * 10;

    return score;
}

/// 按给定掩码生成完整矩阵（功能图形 + 数据 + 掩码 + 格式信息）。
///
/// 注意顺序：掩码只作用于**数据区**，格式信息必须最后写 —— 先写格式再上掩码，
/// 会把 15 位格式信息一起翻掉，扫码器读到的容错等级/掩码号全是错的。
QVector<quint8> buildMatrix(int version, Ecc ecc, const QVector<quint8> &codewords, int mask)
{
    QVector<quint8> modules;
    QVector<quint8> reserved;
    drawBasePatterns(version, modules, reserved);

    const int size = sizeForVersion(version);
    const int totalBits = codewords.size() * 8;

    forEachDataModule(size, reserved, [&](int x, int y, int index) {
        quint8 bit = 0;
        if (index < totalBits)
            bit = static_cast<quint8>((codewords[index >> 3] >> (7 - (index & 7))) & 1);
        modules[y * size + x] = bit;
        return true;
    });

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!reserved[y * size + x] && isMasked(mask, x, y))
                modules[y * size + x] = static_cast<quint8>(modules[y * size + x] ^ 1);
        }
    }

    placeFormat(modules, reserved, size, formatCode(eccFormatBits(ecc), mask));
    return modules;
}

/// 按容量表把数据码字分块，逐块算纠错码字，再按规范交织。
///
/// 交织规则：先按"第 0 块第 0 字节、第 1 块第 0 字节、…"取完所有数据字节
/// （短块在第 0 组末尾，取完就跳过），再按同样顺序取纠错字节。
/// 这样即使某一块整块损坏，损坏也只是散布在码流里的若干字节，不会连成一片。
QVector<quint8> interleave(int version, Ecc ecc, const QVector<quint8> &dataCodewords)
{
    const EccBlockSpec &spec = blocksFor(version, ecc);
    const int ecPerBlock = spec.ecPerBlock;

    struct Block
    {
        QVector<quint8> data;
        QVector<quint8> ec;
    };
    QVector<Block> blocks;
    blocks.reserve(int(spec.blocks1) + int(spec.blocks2));

    int offset = 0;
    const auto appendBlocks = [&](int count, int dataLength) {
        for (int i = 0; i < count; ++i) {
            Block block;
            block.data = dataCodewords.mid(offset, dataLength);
            offset += dataLength;
            block.ec = qrgalois::remainder(block.data, ecPerBlock);
            blocks.append(block);
        }
    };
    appendBlocks(spec.blocks1, spec.data1);
    appendBlocks(spec.blocks2, spec.data2);

    int maxData = 0;
    for (const Block &block : blocks)
        maxData = qMax(maxData, block.data.size());

    QVector<quint8> result;
    result.reserve(totalCodewords(version));
    for (int i = 0; i < maxData; ++i) {
        for (const Block &block : blocks) {
            if (i < block.data.size())
                result.append(block.data.at(i));
        }
    }
    for (int i = 0; i < ecPerBlock; ++i) {
        for (const Block &block : blocks)
            result.append(block.ec.at(i));
    }
    return result;
}

} // namespace

Code encode(const QString &text, const EncodeOptions &options)
{
    Code code;

    // 容错等级越界时钳到默认值：界面上只可能给这四个值，但参数结构体是公开的，
    // 这里挡一下，避免数组下标越界读表。
    Ecc ecc = options.ecc;
    if (int(ecc) < 0 || int(ecc) > int(Ecc::High))
        ecc = Ecc::Medium;

    const QByteArray utf8 = text.toUtf8();
    const int byteLength = utf8.size();

    // ---- 1. 选版本 ----
    int version = 0;
    int capacityBytes = 0;
    for (int v = kMinVersion; v <= kMaxVersion; ++v) {
        const int countBits = byteCountBits(v);
        const int availableBits = totalDataCodewords(v, ecc) * 8;
        const int neededBits = 4 + countBits + byteLength * 8;
        if (neededBits <= availableBits) {
            version = v;
            capacityBytes = (availableBits - 4 - countBits) / 8;
            break;
        }
    }
    if (version == 0) {
        const int maxBits = totalDataCodewords(kMaxVersion, ecc) * 8;
        const int maxBytes = (maxBits - 4 - byteCountBits(kMaxVersion)) / 8;
        code.error = QStringLiteral("内容太长：%1 字节，%2 容错下最多 %3 字节")
                         .arg(byteLength)
                         .arg(eccName(ecc))
                         .arg(maxBytes);
        return code;
    }

    // ---- 2. 拼位流 ----
    const int dataCodewordCount = totalDataCodewords(version, ecc);
    BitBuffer buffer;
    buffer.append(0x4, 4);                       // 0100 = byte 模式
    buffer.append(byteLength, byteCountBits(version));
    for (const char ch : utf8)
        buffer.append(static_cast<quint8>(ch), 8);

    const int capacityBits = dataCodewordCount * 8;
    buffer.append(0, qMin(4, capacityBits - buffer.bitLength));   // 终止符
    while (buffer.bitLength % 8 != 0)                              // 对齐到字节
        buffer.append(0, 1);
    // 交替填充码字 0xEC / 0x11，防止全零数据产生大片同色（规范指定）
    while (buffer.bytes.size() < dataCodewordCount) {
        buffer.bytes.append(0xEC);
        if (buffer.bytes.size() < dataCodewordCount)
            buffer.bytes.append(0x11);
    }

    // ---- 3. 分块 + 纠错 + 交织 ----
    const QVector<quint8> codewords = interleave(version, ecc, buffer.bytes);

    // ---- 4/5. 填矩阵 + 选掩码 ----
    int bestMask = 0;
    int bestScore = std::numeric_limits<int>::max();
    QVector<quint8> best;
    const int size = sizeForVersion(version);
    for (int mask = 0; mask < 8; ++mask) {
        const QVector<quint8> candidate = buildMatrix(version, ecc, codewords, mask);
        const int score = penaltyScore(candidate, size);
        if (score < bestScore) {
            bestScore = score;
            bestMask = mask;
            best = candidate;
        }
    }

    // ---- 6. 收尾 ----
    code.ok = true;
    code.version = version;
    code.mask = bestMask;
    code.size = size;
    code.dataBytes = byteLength;
    code.capacityBytes = capacityBytes;
    code.modules = best;
    return code;
}

int autoScale(const Code &code, const EncodeOptions &options)
{
    if (options.scale > 0)
        return options.scale;
    const int modules = code.size + qMax(0, options.border) * 2;
    if (modules <= 0)
        return 1;
    // 至少 2 像素/模块：1 像素时缩放显示会糊成灰块，扫码器读不出来
    return qMax(2, options.targetPixels / modules);
}

QImage render(const Code &code, const EncodeOptions &options)
{
    if (!code.isValid())
        return QImage();

    const int scale = autoScale(code, options);
    const int border = qMax(0, options.border);
    const int pixels = (code.size + border * 2) * scale;

    QImage image(pixels, pixels, QImage::Format_RGB32);
    image.fill(options.light);

    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(options.dark);
    for (int y = 0; y < code.size; ++y) {
        for (int x = 0; x < code.size; ++x) {
            if (!code.modules.at(y * code.size + x))
                continue;
            // 用整数坐标画实心方块：不开抗锯齿，否则模块边缘会被插值成灰色，
            // 在低分辨率下直接把数据模块糊掉。
            painter.fillRect(border * scale + x * scale,
                             border * scale + y * scale,
                             scale, scale,
                             options.dark);
        }
    }
    painter.end();
    return image;
}

} // namespace qrcode

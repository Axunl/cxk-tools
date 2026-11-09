#pragma once

// ---------------------------------------------------------------------------
// MD5 / HMAC-MD5 的纯逻辑（无 UI、无状态、无全局变量）。
//
// 单独抽成头文件的目的：面板之外的规则（编码、截断、比较）能用临时断言程序
// 直接验证，不必起界面（见 AGENTS.md 的验证流程）。
//
// 三条约定，改之前先读：
//   1) 文本一律按 **UTF-8 字节** 计算。MD5 吃的是字节不是字符，同一串中文在
//      GBK / UTF-8 下摘要完全不同；要与 md5sum、在线工具对齐就只能用 UTF-8。
//   2) 内部一律产出小写十六进制，只有展示层需要大写时才转；比较时忽略大小写。
//   3) 这里不做任何"可逆"暗示：MD5 是单向摘要，无法从摘要还原原文。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace md5logic {

/// 对原始字节求 MD5，返回 16 字节裸摘要（后续再决定转不转十六进制）。
inline QByteArray digest(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Md5);
}

/// 16 字节裸摘要 → 32 位十六进制文本。upper 为 true 时输出大写（A-F）。
inline QString hexOf(const QByteArray &raw, bool upper = false)
{
    const QByteArray hex = raw.toHex();
    return QString::fromLatin1(upper ? hex.toUpper() : hex);
}

/// 字节数组的 MD5（32 位十六进制）。
inline QString hashBytes(const QByteArray &data, bool upper = false)
{
    return hexOf(digest(data), upper);
}

/// 文本的 MD5（32 位十六进制）。文本先转 UTF-8，见文件头约定 1。
inline QString hashText(const QString &text, bool upper = false)
{
    return hashBytes(text.toUtf8(), upper);
}

/// 32 位十六进制 → 16 位形式。
///
/// 16 位 MD5 不是另一种算法，而是 32 位结果去掉前 8 位与后 8 位（取中间 16 个字符），
/// 老版 PHP / Java 系统里很常见。输入长度不是 32 时原样返回，避免悄悄截断错误数据。
inline QString shortDigest(const QString &hex32)
{
    return hex32.size() == 32 ? hex32.mid(8, 16) : hex32;
}

/// HMAC-MD5（RFC 2104），返回 16 字节裸摘要。
///
/// 用途：很多国内开放平台的接口签名是 hmac_md5(key, 拼接串)，调试时需要一个
/// 不联网、可复核的实现。密钥超过 64 字节要先哈希一次（RFC 2104 的规定），
/// 不足 64 字节右侧补 0。
inline QByteArray hmacMd5Raw(const QByteArray &key, const QByteArray &message)
{
    constexpr int kBlockSize = 64;   // MD5 的分组长度，固定 64 字节，不是可调参数
    constexpr char kIpad = 0x36;
    constexpr char kOpad = 0x5C;

    QByteArray normalizedKey = key;
    if (normalizedKey.size() > kBlockSize)
        normalizedKey = digest(normalizedKey);
    // 显式指定填充字节：QByteArray::resize(n) 新增的字节是**未初始化**的，
    // 直接 resize 会把栈/堆上的垃圾拼进 ipad/opad，摘要全错（实测出来的坑）。
    // RFC 2104 要求不足 64 字节时右侧补 0。
    normalizedKey.resize(kBlockSize, '\0');

    QByteArray innerPad(kBlockSize, '\0');
    QByteArray outerPad(kBlockSize, '\0');
    for (int i = 0; i < kBlockSize; ++i) {
        innerPad[i] = static_cast<char>(normalizedKey.at(i) ^ kIpad);
        outerPad[i] = static_cast<char>(normalizedKey.at(i) ^ kOpad);
    }

    const QByteArray inner = digest(innerPad + message);
    return digest(outerPad + inner);
}

/// HMAC-MD5 的十六进制形式（密钥与文本都按 UTF-8 字节处理）。
inline QString hmacMd5(const QByteArray &key, const QByteArray &message, bool upper = false)
{
    return hexOf(hmacMd5Raw(key, message), upper);
}

/// 文本输入的便捷重载。
inline QString hmacMd5Text(const QString &key, const QString &text, bool upper = false)
{
    return hmacMd5(key.toUtf8(), text.toUtf8(), upper);
}

/// 期望值与计算结果是否一致：去首尾空白后忽略大小写比较。
///
/// 之所以忽略大小写而不是严格相等：用户从日志/文档里粘过来常常是大写，
/// 而大小写不影响摘要本身。空期望值一律视为"还没填"，返回 false。
inline bool matches(const QString &expected, const QString &actual)
{
    const QString trimmed = expected.trimmed();
    if (trimmed.isEmpty())
        return false;
    return trimmed.compare(actual, Qt::CaseInsensitive) == 0;
}

} // namespace md5logic

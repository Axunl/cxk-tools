#pragma once

// ---------------------------------------------------------------------------
// 二维码解码（输入一张 QImage，输出内容与元信息）。
//
// 只做"读"：定位 → 透视校正 → 采样 → 读格式信息 → 反掩码 → 纠错 → 解析位流。
// 不依赖任何第三方库，也不联网；全部算法在本目录的 QrDecode.cpp 里，
// 编码用的常量表与布局规则复用 QrTables.h / QrLayout.h。
//
// 支持范围（与 README 的「已知边界」一致）：
//   * 版本 1–40，容错 L/M/Q/H，掩码 0–7；byte / 数字 / 字母数字 / ECI 段；
//   * 图片可以旋转、缩放、有轻度透视（校正图形可用时按四点透视变换采样）；
//   * 不支持：镜像（前置摄像头自拍）、Kanji（Shift-JIS）段、结构化的多张拼接。
// ---------------------------------------------------------------------------

#include <QImage>
#include <QString>

namespace qrcode {

struct DecodeResult
{
    bool ok = false;
    QString text;                 ///< 解出的文本（ok == false 时为空）
    QString error;                ///< 失败原因（中文，直接显示给用户）

    int version = 0;              ///< 1–40
    int mask = -1;                ///< 掩码 0–7
    QString eccName;              ///< "L" / "M" / "Q" / "H"
    int correctedCodewords = 0;   ///< 纠错实际改动的码字数（0 = 图片本身完好）
};

////
/// 从图片里读取二维码。
///
/// 内部会先试全局阈值，失败后再试局部阈值（照片光照不均时用），
/// 并在估算版本附近多试几个版本 —— 模块尺寸的估计误差 ±1 个版本很常见。
///
/// @return 失败时 ok == false，error 是给用户看的原因，不会抛异常
///
DecodeResult decodeImage(const QImage &image);

} // namespace qrcode

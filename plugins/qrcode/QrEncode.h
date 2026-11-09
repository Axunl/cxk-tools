#pragma once

// ---------------------------------------------------------------------------
// 二维码编码（纯逻辑 + 一张 QImage，不碰 QWidget，可单独写断言验证）。
//
// 实现范围：byte 模式（UTF-8），版本 1–40 自动选择，容错 L/M/Q/H。
// 为什么只做 byte 模式：本项目的内容是用户随手输入的文本 / 网址 / 中文，
// 全都能用 byte 模式表达；numeric / alphanumeric 只是省容量，不会让"能不能生成"
// 发生变化，却要多维护两套位流拼装与校验逻辑，收益不成比例。
//
// 调用方式：
//     const qrcode::Code code = qrcode::encode(text, options);
//     if (!code.ok)  ... code.error 是给用户看的中文原因
//     const QImage img = qrcode::render(code, options);
// ---------------------------------------------------------------------------

#include "QrTables.h"

#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>

namespace qrcode {

/// 生成参数。默认值就是界面上的默认选项（容错 M、4 模块静区、深蓝黑而非纯黑）。
struct EncodeOptions
{
    Ecc ecc = Ecc::Medium;
    int scale = 0;             ///< 每模块像素数；0 = 按 targetPixels 自动算
    int border = 4;            ///< 静区模块数，规范要求 >= 4（少了扫码器可能贴边失败）
    int targetPixels = 340;    ///< 自动 scale 时的目标边长（像素）
    QColor dark = QColor(0x0F, 0x17, 0x2A);
    QColor light = QColor(0xFF, 0xFF, 0xFF);
};

/// 一次编码的产物。modules 是**不含静区**的纯矩阵，渲染与测试都用它。
struct Code
{
    bool ok = false;
    QString error;               ///< ok == false 时的中文原因

    int version = 0;             ///< 1–40
    int mask = 0;                ///< 实际选中的掩码 0–7
    int size = 0;                ///< 矩阵边长（模块数，不含静区）
    int dataBytes = 0;           ///< 内容占用的 UTF-8 字节数
    int capacityBytes = 0;       ///< 当前版本+容错等级能放的最大字节数

    QVector<quint8> modules;     ///< size*size，1 = 深色

    bool isValid() const { return ok && size > 0 && modules.size() == size * size; }
};

////
/// 编码一段文本。
///
/// @param text    任意文本；内部按 UTF-8 转字节（中文一个字 3 字节，会吃掉容量）
/// @param options 容错等级等参数
/// @return ok == false 时 error 说明原因（内容过长 / 参数非法），不会抛异常
///
Code encode(const QString &text, const EncodeOptions &options = EncodeOptions());

/// 把矩阵渲染成图片（含静区）。code 不合法时返回空 QImage。
QImage render(const Code &code, const EncodeOptions &options = EncodeOptions());

/// 按参数算出实际使用的模块像素（界面显示"每模块 N 像素"用同一个数）。
int autoScale(const Code &code, const EncodeOptions &options = EncodeOptions());

} // namespace qrcode

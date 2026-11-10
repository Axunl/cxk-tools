#pragma once

// ---------------------------------------------------------------------------
// 拼音数据表的声明（表体在 PinyinData.cpp，由 scripts/gen-pinyin-data.py 生成）。
//
// 单独拆一个头文件是为了让「码位区间」这一个关键约束只出现一次：
// 生成脚本、查表代码、覆盖率注释都引用这里的常量，改区间时不会漏掉某一处。
// ---------------------------------------------------------------------------

#include <QtGlobal>

namespace pinyin {
namespace data {

/// 表覆盖的第一个码位（CJK 基本区起）。
constexpr ushort kFirstCodePoint = 0x4E00;
/// 表覆盖的最后一个码位（含端点）。
constexpr ushort kLastCodePoint = 0x9FFF;
/// 表项数 = 码位个数，两者严格一一对应（脚本里有断言）。
constexpr int kEntryCount = kLastCodePoint - kFirstCodePoint + 1;

/// 空格分隔的拼音表，按码位升序，缺读音的项写 "-"。
/// 以 NUL 结尾（字符串字面量），长度用 kEntryCount 推不出来，遍历时按 '\0' 截止。
extern const char kTable[];

} // namespace data
} // namespace pinyin

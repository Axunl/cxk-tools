#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ---------------------------------------------------------------------------
# 生成 src/core/PinyinData.cpp：汉字 → 无声调拼音 的静态表。
#
# 为什么要有这个脚本：首页搜索要支持「打 wx 找到微信」，就得知道每个汉字怎么念。
# 引入 libpinyin / pypinyin 之类的运行期依赖对一个纯 Qt 的托盘小工具来说太重，
# 所以把「每个字一个读音」的表编进可执行文件里，运行时只做码位查表。
#
# 用法（需要 pypinyin，仅在生成时用，不参与构建）：
#     python scripts/gen-pinyin-data.py
#
# 生成规则：
#   * 覆盖 U+4E00..U+9FFF（CJK 基本区，20992 个码位），与 PinyinData.h 里的常量一致；
#   * 每个码位存「主读音 + 一个次读音」（多音字才需要，用 '|' 分隔）：主读音用于
#     全拼/缩写，次读音让 乐(yue)/重(chong)/长(zhang) 这类字也能被另一种拼法搜到；
#   * 没有读音的码位（未分配、生僻字）写 "-" 占位，保证表项与码位严格一一对应；
#   * 项之间用单个空格分隔，按码位升序 —— 运行时用 index = 码位 - 0x4E00 直接定位。
#
# 表体约 110 KB，全部是 ASCII，存成 C++ 字符串字面量时分块拼接：
# MSVC 对单个字符串字面量有 16380 字节上限，不切块会直接编译报错。
# ---------------------------------------------------------------------------

import sys
from pathlib import Path

FIRST = 0x4E00
LAST = 0x9FFF          # 含端点
CHUNK_BYTES = 12000    # 单块字面量的目标长度，留足余量给 MSVC 的 16380 上限
OUT_PATH = Path(__file__).resolve().parent.parent / "src" / "core" / "PinyinData.cpp"

HEADER = """// ---------------------------------------------------------------------------
// 由 scripts/gen-pinyin-data.py 生成，**请勿手工编辑**。
//
// 数据来源：pypinyin（单字读音，无声调，小写；多音字额外带一个次读音，用 '|' 分隔）。
// 需要改动生成规则时，改脚本再重新跑一遍，不要直接改本文件。
//
// 表结构：与码位 U+4E00..U+9FFF 一一对应（共 {count} 项），项之间用单个空格分隔，
// 缺读音的位置写 "-"。按码位连续存放，是为了让查询退化成一次下标运算
// （index = 码位 - 0x4E00），不必为两百多 MB 机器上的每个字建哈希表。
// ---------------------------------------------------------------------------

#include "PinyinData.h"

namespace pinyin {{
namespace data {{

// MSVC 单个字符串字面量上限 16380 字节，这里分块再靠相邻字面量拼接。
const char kTable[] =
"""


def build_entries():
    """返回与 [FIRST, LAST] 对齐的读音串列表，缺读音为 '-'。

    每一项形如 "le" 或 "le|yue"：竖线前是主读音，竖线后是多音字的**一个**次读音。
    为什么要带次读音：中文名里 乐/重/长/行 这类字的常用音经常不是主读音，
    「网易云音乐」按主读音得到 yinle，用户打 yinyue 就搜不到。带一个次读音后
    搜索层可以再拿另一套全拼/缩写比一次，成本只有两次字符串比较。
    """
    try:
        from pypinyin import Style, pinyin
    except ImportError:
        sys.exit("需要 pypinyin：pip install pypinyin")

    entries = []
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        # heteronym=True 会把多音字的读音都给出（按常见度排序）；
        # errors 回调保证查不到的字返回空串而不是原字符
        result = pinyin(ch, style=Style.NORMAL, heteronym=True,
                        errors=lambda x: ["" for _ in x])
        readings = []
        for raw in (result[0] if result else []):
            if raw and raw != ch and raw.isascii() and raw not in readings:
                readings.append(raw)
        if not readings:
            entries.append("-")
        elif len(readings) == 1:
            entries.append(readings[0])
        else:
            # 只留一个次读音：读音再多也没有匹配价值，反而把表撑大
            entries.append(readings[0] + "|" + readings[1])
    return entries


def main():
    entries = build_entries()
    assert len(entries) == LAST - FIRST + 1, "表项数与码位区间不一致"

    lines = []
    chunk = ""
    for entry in entries:
        piece = entry + " "
        if len(chunk) + len(piece) > CHUNK_BYTES:
            lines.append(chunk)
            chunk = ""
        chunk += piece
    if chunk:
        lines.append(chunk)

    body = []
    for index, line in enumerate(lines):
        # 除最后一块外都要保留块尾那个空格：相邻字面量是直接拼接的，
        # 块尾空格剁掉会把上一块最后一个音节和下一块第一个音节粘成一个词。
        piece = line if index < len(lines) - 1 else line.rstrip()
        # 拼音只含 a-z 与 '-'，行内不会出现引号/反斜杠，无需转义
        body.append('    "%s"\n' % piece)

    text = HEADER.format(count=len(entries)) + "".join(body) + ";\n\n} // namespace data\n} // namespace pinyin\n"
    # 统一 LF + UTF-8 无 BOM：仓库规则 5，混入 CRLF 会让 diff 全是噪音
    OUT_PATH.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s (%d entries, %d bytes)" % (OUT_PATH, len(entries), len(text)))


if __name__ == "__main__":
    main()

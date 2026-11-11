#pragma once

// ---------------------------------------------------------------------------
// 首页搜索框的「就地求值」（纯逻辑，不依赖 QWidget，可写临时断言程序验证）。
//
// 解决的问题：用户想算 12*8，不该为此打开计算器面板再敲一遍。这一层回答
// 「这段输入是不是一道算式、算出来是多少」，界面由 HomePage 负责把结果摆进
// 搜索框右侧的徽标里（回车/点击即复制）。
//
// 为什么单独一层而不是直接在 HomePage 里调 calc::ExprEval：
//   1. 求值得先判断「该不该求」—— 搜索框里绝大多数输入是关键词，对着它们
//      每敲一个键跑一遍解析器既浪费，也会把 "3d"、"4k" 这类词误判成算式；
//   2. 判定规则（至少两位数字、至少一个运算符、长度上限）是可单测的纯逻辑，
//      放在头文件里能脱离界面直接验证，符合 AGENTS.md 规则 2 的验证方式。
//
// 求值本身复用 api/ExprEval.h：与计算器面板同一份实现，两边不会给出不同答案。
// ---------------------------------------------------------------------------

#include "ExprEval.h"

#include <QString>

namespace quickcalc {

/// 判定用的长度上限。搜索框是单行输入框，正常算式不会这么长；
/// 设上限是为了避免「粘了一整段文本」时每敲一个键都去做一次全量解析。
constexpr int kMaxChars = 120;

/// 一次就地求值的结果。ok == false 时其余字段无意义（error 仅用于日志排查）。
struct Result
{
    bool    ok = false;
    double  value = 0.0;
    QString text;    ///< 已格式化的结果（不带 "="，可直接进剪贴板）
    QString error;
};

/// 这段输入是否「值得当算式算一遍」。
///
/// 判定刻意收紧，因为误判的代价比漏判大得多：搜索框里多出一个莫名其妙的结果，
/// 用户会以为工具坏了。三条同时满足才算：
///   1. 含**至少一位**数字 —— 纯英文（含 `pi` / `e` 这种常量）不当算式，
///      「输入英文就冒出个数字」是最容易被当成 bug 的一种表现；
///   2. 含至少一个运算符或括号，且**开头的正负号不算** —— 排除纯搜索词
///      （`3d`、`json`），也排除 `-5`、`+3` 这种「本来就是个数字」的输入
///      （它们算出来等于自己，除了噪音没有任何信息）；
///   3. 长度不超过 kMaxChars。
/// 剩下的（未知名字、语法错误、除零）交给 ExprEval 判定：只认 pi / e 两个名字，
/// 别的字母一律报错，所以 `mp3`、`mp4+1` 不会被当成算式。
inline bool looksLikeExpression(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxChars)
        return false;

    int digits = 0;
    bool hasBinaryOperator = false;
    for (int i = 0; i < trimmed.size(); ++i) {
        const QChar ch = trimmed.at(i);
        if (ch.isDigit()) {
            ++digits;
            continue;
        }
        switch (ch.unicode()) {
        case '*':
        case '/':
        case '%':
        case '^':
        case '(':
        case ')':
            hasBinaryOperator = true;
            break;
        case '+':
        case '-':
            // 位置 0 的 +/− 是一元符号（`-5`），不是二元运算
            if (i > 0)
                hasBinaryOperator = true;
            break;
        default:
            break;
        }
    }

    return digits >= 1 && hasBinaryOperator;
}

/// 就地求值。非法输入 / 语法错误 / 结果溢出都返回 ok == false（不抛异常）。
inline Result evaluate(const QString &text)
{
    Result result;
    if (!looksLikeExpression(text))
        return result;

    if (calc::ExprEval(text.trimmed()).evaluate(&result.value, &result.error)) {
        result.ok = true;
        result.text = calc::formatNumber(result.value);
        result.error.clear();
    }
    return result;
}

} // namespace quickcalc

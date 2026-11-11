#pragma once

// ---------------------------------------------------------------------------
// JS 运行插件的「控制台」预置片段与输出限制。
//
// 为什么 console 要自己装：QJSEngine 默认不提供任何 logging 工具（官方文档：
// 「by default, familiar utilities like logging are not available」），而脚本
// 最主要的产出手段就是 console.log。Qt 自带的 ConsoleExtension 会把日志打到
// stderr，抓不回界面，所以这里自己装一个只认两段字符串的桥（见 JsEngineRunner.h）。
//
// 为什么格式化写在 JS 里而不是 C++ 里：值在 JS 侧还能访问结构（对象/数组/循环
// 引用），转成 QString 之后就只剩 toString 的残骸。同一份 describe() 既用于
// console.log 的每个参数，也用于格式化脚本最终返回值，两处规则不会漂移。
// ---------------------------------------------------------------------------

#include <QString>

namespace jsrun {

/// 单条日志的最大字符数。超过就截断——脚本里 `console.log(bigString)` 很常见，
/// 不截断会让主线程忙于渲染几十万字的 QTextDocument，界面直接卡住。
inline constexpr int kMaxLogLineChars = 4000;

/// 单次运行最多往界面推多少条日志。超出的直接丢弃并补一条说明：
/// 排队到主线程的 queued 事件本身就是负担，刷屏脚本（循环里 log）比卡顿更糟。
inline constexpr int kMaxConsoleLines = 1000;

/// 最终返回值的最大字符数。
inline constexpr int kMaxResultChars = 20000;

/// 安装 console 的 IIFE。接收 console 桥作为参数（**不写全局变量**，脚本执行
/// 时拿不到任何宿主对象），返回 `{ logger: 控制台对象, describe: 格式化函数 }`，
/// 由 C++ 侧挂到 engine.globalObject() 上。
///
/// 为什么不在这里直接 `globalThis.console = …`：实测本机 Qt 6.8.3 的 V4 **没有
/// `globalThis`**（`typeof globalThis` 是 undefined，直接引用会抛 ReferenceError）。
/// 之前靠非严格模式下 `this` 指着全局对象「碰巧能用」，那是实现细节，不能依赖；
/// 挂全局这件事交给 QJSEngine::globalObject() 才是稳定写法。
///
/// 注意 WeakSet 的语义：它记录「已经出现过」的对象，所以同一个对象出现两次
/// （即使不是环）第二次也会显示成 [循环引用]。这是有意的取舍——真正的环必须
/// 拦住（否则 JSON.stringify 直接抛错，用户看不到任何内容），而少数「共享引用
/// 被误标」的场景不影响阅读。
inline const char *const kConsoleInstaller = R"JS((function (bridge) {
    "use strict";

    // 先抓住原始实现：脚本后面改写 JSON.stringify / WeakSet 不影响显示
    var stringify = JSON.stringify;
    var hasWeakSet = (typeof WeakSet === "function");

    function describe(value) {
        if (value === void 0) return "undefined";
        if (value === null) return "null";
        var type = typeof value;
        if (type === "string") return value;          // 顶层字符串不加引号
        if (type === "number" || type === "boolean") return String(value);
        if (type === "bigint") return String(value) + "n";
        if (type === "symbol") return String(value);
        if (type === "function")
            return "[Function" + (value.name ? ": " + value.name : "") + "]";

        var text = "";
        try {
            if (hasWeakSet) {
                var seen = new WeakSet();
                text = stringify(value, function (key, inner) {
                    if (inner && typeof inner === "object") {
                        if (seen.has(inner)) return "[循环引用]";
                        seen.add(inner);
                    }
                    return inner;
                }, 2);
            } else {
                text = stringify(value, null, 2);
            }
        } catch (e) {
            // 环（或 BigInt 之类 stringify 不了的值）：退回 toString，至少不是空白
            try { text = String(value); } catch (e2) { text = "[无法显示的值]"; }
        }
        if (typeof text !== "string" || text.length === 0) {
            try { text = String(value); } catch (e3) { text = "[无法显示的值]"; }
        }
        return text;
    }

    function emit(level) {
        return function () {
            var parts = [];
            for (var i = 0; i < arguments.length; i++) parts.push(describe(arguments[i]));
            // 多参数用单个空格连接：与浏览器 / Node.js 的观感一致，不做 %s 占位符替换
            bridge.log(level, parts.join(" "));
        };
    }

    var logger = {
        log: emit("log"),
        debug: emit("log"),
        info: emit("log"),
        warn: emit("warn"),
        error: emit("error")
    };

    return { logger: logger, describe: describe };
}))JS";

/// 把超长文本截断成「前 maxChars 字 + 说明」。注释里的数字由调用点决定，
/// 统一在这里做是为了所有出口（日志、返回值）截断文案一致。
inline QString truncateForUi(const QString &text, int maxChars)
{
    if (maxChars <= 0 || text.size() <= maxChars)
        return text;
    return text.left(maxChars) + QStringLiteral(" …（内容过长，已截断）");
}

} // namespace jsrun

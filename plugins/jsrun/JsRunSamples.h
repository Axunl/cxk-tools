#pragma once

// ---------------------------------------------------------------------------
// 内置示例片段：给第一次打开工具的人一个「点一下就能看到效果」的入口。
//
// 选这 6 条的原则：覆盖 JS 日常用途里最常见的几类（数组、对象、正则、时间、
// 造数据），并留一条死循环用来演示超时中断——那是这个工具最容易被误解的行为，
// 让用户亲手点一次比在文档里写十行更有效。
//
// 每条示例都会被执行验证（临时工程里逐条跑过，见 AGENTS.md 的验证流程），
// 所以它们同时也是「引擎能力清单」：能跑通这些，说明 ES7 常用语法都在。
// ---------------------------------------------------------------------------

#include <QString>
#include <QVector>

namespace jsrun {

struct Sample
{
    QString title; ///< 下拉框里显示的名字
    QString code;  ///< 载入编辑器的完整脚本
};

inline QVector<Sample> samples()
{
    return {
        {QStringLiteral("数组：筛选 / 映射 / 合计"),
         QString::fromUtf8(R"JS(const nums = [5, 3, 9, 1, 7];

const evens = nums.filter(n => n % 2 === 0);
const doubled = nums.map(n => n * 2);
const sum = nums.reduce((a, b) => a + b, 0);

console.log("原数组", nums);
console.log("偶数", evens);
console.log("各乘 2", doubled);

// 最后一行表达式的值会作为「返回值」显示在结果块里
({ 合计: sum, 最大值: Math.max(...nums) })
)JS")},

        {QStringLiteral("对象：嵌套结构与 JSON"),
         QString::fromUtf8(R"JS(const user = {
    name: "张三",
    tags: ["dev", "qt"],
    profile: { city: "杭州", since: 2021 }
};

console.log("用户信息", user);
console.log("标签数", user.tags.length);

// 返回字符串时按原样显示，不会再套一层引号
JSON.stringify(user, null, 2)
)JS")},

        {QStringLiteral("正则：从文本里抽取订单"),
         QString::fromUtf8(R"JS(const text = "订单 A-1024 金额 128.50 元，订单 B-2048 金额 64.00 元";
const pattern = /订单 ([A-Z]-\d+) 金额 ([\d.]+) 元/g;

const rows = [];
let match;
while ((match = pattern.exec(text)) !== null) {
    rows.push({ 单号: match[1], 金额: Number(match[2]) });
}

console.log("共解析", rows.length, "条");
rows
)JS")},

        {QStringLiteral("时间：格式化当前时间"),
         QString::fromUtf8(R"JS(function pad(n) {
    return n < 10 ? "0" + n : String(n);
}

const now = new Date();
const stamp = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} `
            + `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;

console.log("现在", stamp);
console.log("距今天已过毫秒", now.getTime() % 86400000);

({ 时间: stamp, 星期: now.getDay(), 时间戳: now.getTime() })
)JS")},

        {QStringLiteral("造数据：生成对象数组"),
         QString::fromUtf8(R"JS(const people = [];
for (let i = 1; i <= 5; ++i) {
    people.push({ id: i, name: `用户${i}`, score: (i * 17) % 40 + 60 });
}

const total = people.reduce((sum, p) => sum + p.score, 0);
console.log("平均分", (total / people.length).toFixed(1));

// 按分数从高到低排序
people.sort((a, b) => b.score - a.score)
)JS")},

        {QStringLiteral("死循环：验证 3 秒超时中断"),
         QString::fromUtf8(R"JS(// 这段代码永远不会结束，用来验证超时保护：
// 点「运行」后 3 秒会提示「执行超时（>3s），已中断」，界面不会卡死，
// 也随时可以切回首页（面板销毁时会先打断执行线程再退出）。
let count = 0;
while (true) {
    count++;
}
)JS")},
    };
}

/// 首次进入工具时载入哪一条。固定数组第一项，不随机——用户第二次打开时
/// 看到的应该还是同一段，便于对照。
inline QString firstSampleCode()
{
    const QVector<Sample> all = samples();
    return all.isEmpty() ? QStringLiteral("1 + 1") : all.first().code;
}

} // namespace jsrun

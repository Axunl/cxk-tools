#pragma once

// ---------------------------------------------------------------------------
// 随机密码生成的纯逻辑（无 UI、无状态、无全局变量）。
//
// 单独抽成头文件，是为了把「怎么算」和「怎么画」分开：字符集、熵值、
// 强度分级、采样算法都能用临时断言程序验证，不必起界面（见 AGENTS.md 规则 2）。
//
// 三条硬约定，改之前先读：
//   1) 随机源只用 QRandomGenerator::system()（操作系统 CSPRNG：Windows 是
//      RtlGenRandom，Linux/macOS 是 /dev/urandom 或对应系统调用）。
//      **绝不能用 qrand / QRandomGenerator::global()** —— 后者是可预测的
//      伪随机数，用来生成密码等于没有密码。
//   2) 取下标一律走 uniformBelow() 的拒绝采样，不用裸取模：2^32 不是字符集
//      大小的整数倍时，取模会让排在前面的字符概率偏高（模偏差），直接削弱熵。
//   3) 逻辑层不返回颜色、不返回 UI 文案以外的样式，强度只给枚举，配色由面板决定，
//      这样换主题不会牵动这里。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QRandomGenerator>
#include <QString>
#include <QStringList>

#include <cmath>

namespace pwdgen {

/// 界面与逻辑共用的长度范围。下限 4 是「再短就没有意义」的实用底线，
/// 上限 128 是很多站点/系统的接受上限，再长面板也放不下。
inline constexpr int kMinLength = 4;
inline constexpr int kMaxLength = 128;

/// 一组生成参数。默认值刻意选成「16 位、四类字符全要、排除易混淆、每类至少一个」，
/// 打开工具第一眼看到的就是一个能直接用的强密码。
struct Options
{
    int  length           = 16;   ///< 密码长度（字符个数）
    bool lower            = true; ///< 包含小写字母 a-z
    bool upper            = true; ///< 包含大写字母 A-Z
    bool digits           = true; ///< 包含数字 0-9
    bool symbols          = true; ///< 包含符号（见 symbolChars()）
    bool excludeAmbiguous = true; ///< 排除 0 O 1 l I | 这类肉眼难分的字符
    bool requireEachClass = true; ///< 勾选的每一类至少出现一次
};

/// 强度分级。阈值定义在 strengthOf()，面板据此选色与文案。
enum class Strength
{
    Weak,       ///< < 45 位熵：只够应付不重要的临时注册
    Fair,       ///< 45–65：能挡在线爆破，挡不住离线撞库
    Strong,     ///< 65–90：日常账号够用
    VeryStrong, ///< ≥ 90：重要账号 / 主密码
};

inline const QString &lowerChars()
{
    static const QString set = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
    return set;
}

inline const QString &upperChars()
{
    static const QString set = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    return set;
}

inline const QString &digitChars()
{
    static const QString set = QStringLiteral("0123456789");
    return set;
}

/// 符号集刻意去掉反引号、单引号、双引号、反斜杠与美元符：
/// 这些字符粘进 shell、SQL、JSON、.env 时都要额外转义，用户复制过去
/// 十有八九会因为转义问题登录失败。剩下的符号一样能把熵拉满。
inline const QString &symbolChars()
{
    static const QString set = QStringLiteral("!@#%^&*()-_=+[]{};:,.?/~<>|");
    return set;
}

/// 易混淆字符：数字 0 与字母 O、数字 1 与字母 l/I、以及竖线。
/// 印刷体/某些等宽字体里几乎一样，手抄到另一台设备上最容易错的就是它们。
inline const QString &ambiguousChars()
{
    static const QString set = QStringLiteral("0O1lI|");
    return set;
}

/// 去掉 text 里出现在 remove 中的每个字符。
inline QString withoutChars(const QString &text, const QString &remove)
{
    if (remove.isEmpty())
        return text;
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (!remove.contains(ch))
            out.append(ch);
    }
    return out;
}

/// 当前参数下实际参与生成的非空字符集（已按需剔除易混淆字符）。
/// 返回顺序固定为 小写 → 大写 → 数字 → 符号，方便「每类至少一个」时定位来源。
inline QStringList selectedSets(const Options &options)
{
    QStringList sets;
    const QString remove = options.excludeAmbiguous ? ambiguousChars() : QString();
    auto add = [&sets, &remove](const QString &set) {
        const QString filtered = withoutChars(set, remove);
        if (!filtered.isEmpty())
            sets.append(filtered);
    };
    if (options.lower)
        add(lowerChars());
    if (options.upper)
        add(upperChars());
    if (options.digits)
        add(digitChars());
    if (options.symbols)
        add(symbolChars());
    return sets;
}

/// 合并去重后的候选字符全集。去重是为了防止将来往字符集里加重叠字符后，
/// 某些字符被悄悄赋予更高权重（那会降低实际熵，而且很难看出来）。
inline QString alphabet(const Options &options)
{
    QString out;
    for (const QString &set : selectedSets(options)) {
        for (const QChar ch : set) {
            if (!out.contains(ch))
                out.append(ch);
        }
    }
    return out;
}

///
/// 校验参数组合是否可生成。失败时把原因写进 error（面向用户的中文文案），返回 false。
///
/// 两类错误：一类都没勾（没有字符可用）；勾了「每类至少一个」但长度不够
/// （4 类字符至少要点 4 位，否则必然满足不了约束 —— 这里宁可报错也不偷偷放宽约束，
/// 否则用户拿到的是「看起来符合设置、其实缺一类」的密码）。
///
inline bool validate(const Options &options, QString *error = nullptr)
{
    QString dummy;
    QString &err = error ? *error : dummy;
    err.clear();

    if (options.length < kMinLength || options.length > kMaxLength) {
        err = QStringLiteral("长度需要在 %1–%2 之间").arg(kMinLength).arg(kMaxLength);
        return false;
    }

    const QStringList sets = selectedSets(options);
    if (sets.isEmpty()) {
        err = QStringLiteral("至少勾选一类字符");
        return false;
    }

    if (options.requireEachClass && options.length < sets.size()) {
        err = QStringLiteral("勾了 %1 类字符并开启「每类至少一个」时，长度至少要有 %1 位")
                  .arg(sets.size());
        return false;
    }

    return true;
}

///
/// 返回 [0, n) 上的均匀随机整数。
///
/// 为什么不用 generate() % n：n 不整除 2^32 时，落在 [0, 2^32 % n) 的那段区间
/// 会多分到一个样本，前面几个字符的概率偏高。这里把「最后一个不完整区间」
/// 的样本全部丢掉重抽（拒绝采样），偏差为 0。丢弃概率最大不到 50%，
/// 循环期望次数 < 2，性能不是问题。
///
inline quint32 uniformBelow(quint32 n)
{
    if (n <= 1)
        return 0;

    const quint64 range = quint64(1) << 32;   // 32 位随机源能取到的值个数
    const quint64 limit = range - (range % n);
    quint32 value = 0;
    do {
        value = QRandomGenerator::system()->generate();
    } while (quint64(value) >= limit);
    return value % n;
}

/// Fisher–Yates 洗牌（原地）。必须洗：先按类别各取一个再补满的写法，
/// 结果必然是「每类字符集中在前几位」，一看就知道密码是怎么拼的。
inline void shuffle(QString &text)
{
    for (int i = text.size() - 1; i > 0; --i) {
        const int j = static_cast<int>(uniformBelow(static_cast<quint32>(i + 1)));
        // QString 没有 swapItemsAt（那是 QList 的接口），手写三步交换。
        // Qt6 的非 const operator[] 返回 QChar&，可以直接赋值。
        const QChar tmp = text.at(i);
        text[i] = text.at(j);
        text[j] = tmp;
    }
}

///
/// 按参数生成一个密码。参数非法时返回空串并写 error（面板据此提示用户）。
///
/// 生成步骤：
///   1) 勾了「每类至少一个」→ 先从每个字符集各抽一个，保证约束成立；
///   2) 其余位置从合并字符全集里均匀抽；
///   3) 整体洗牌，打散「每类各一个」留下的可识别前缀。
///
inline QString generate(const Options &options, QString *error = nullptr)
{
    QString err;
    if (!validate(options, &err)) {
        if (error)
            *error = err;
        return QString();
    }
    if (error)
        error->clear();

    const QStringList sets = selectedSets(options);
    const QString pool = alphabet(options);

    QString password;
    password.reserve(options.length);

    if (options.requireEachClass) {
        for (const QString &set : sets) {
            const int index = static_cast<int>(uniformBelow(static_cast<quint32>(set.size())));
            password.append(set.at(index));
        }
    }

    while (password.size() < options.length) {
        const int index = static_cast<int>(uniformBelow(static_cast<quint32>(pool.size())));
        password.append(pool.at(index));
    }

    shuffle(password);
    return password;
}

/// 熵值（比特）：log2(字符集大小) × 长度。
///
/// 勾了「每类至少一个」时，实际可用空间比这个值略小（要扣掉缺少某类字符的排列），
/// 所以这是**上界**。差值通常不到 1 位，不影响强弱判断，面板文案写「约」。
inline double entropyBits(int length, int alphabetSize)
{
    if (length <= 0 || alphabetSize <= 1)
        return 0.0;
    return static_cast<double>(length) * std::log2(static_cast<double>(alphabetSize));
}

/// 便捷重载：直接按参数算熵（长度非法或没选字符时返回 0）。
inline double entropyBits(const Options &options)
{
    const QString pool = alphabet(options);
    return entropyBits(options.length, pool.size());
}

/// 熵值 → 强度分级。阈值参考「离线爆破 10^12 次/秒」的粗略直觉，
/// 不是精确的安全结论：45 位≈35 分钟、65 位≈1 年、90 位≈3 万年。
inline Strength strengthOf(double bits)
{
    if (bits < 45.0)
        return Strength::Weak;
    if (bits < 65.0)
        return Strength::Fair;
    if (bits < 90.0)
        return Strength::Strong;
    return Strength::VeryStrong;
}

/// 强度分级 → 中文标签（面板直接用，避免每个调用点各写一份）。
inline QString strengthLabel(Strength level)
{
    switch (level) {
    case Strength::Weak:
        return QStringLiteral("弱");
    case Strength::Fair:
        return QStringLiteral("中等");
    case Strength::Strong:
        return QStringLiteral("强");
    case Strength::VeryStrong:
        return QStringLiteral("很强");
    }
    return QString();
}

} // namespace pwdgen

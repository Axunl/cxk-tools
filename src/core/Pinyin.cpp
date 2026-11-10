#include "Pinyin.h"

#include "PinyinData.h"

#include <QVector>

namespace {

/// ASCII 字母或数字：这类字符在名称里本身就是「已经写好的缩写」，原样参与匹配。
/// 其余非汉字字符（空格、标点、emoji、全角符号）一律当词边界。
bool isAsciiAlnum(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}

/// 表体的索引：把「空格分隔」的静态表解析成每项起始偏移，之后查一个字 = 一次下标 +
/// 构造一个 QLatin1StringView。
///
/// 为什么不建 QHash<QChar, QString>：两万多个字的哈希表在 64 位下要吃掉近 1 MB，
/// 而这个工具是个常驻托盘的搜索框，为「万一用户打拼音」常驻一份哈希表不划算。
/// 代价只是首次使用时扫一遍 86 KB 的表体（毫秒级，且只在第一次搜索时发生）。
struct TableIndex
{
    /// start[i] = 第 i 项在表体里的起始字节偏移；长度靠 start[i+1] 反推。
    /// 表项数少于码位数时（表被改坏）length 会小于 kEntryCount+1，查询侧据此兜底。
    QVector<quint32> start;
    /// 表体总长（不含结尾 NUL），最后一项的长度靠它算（最后一项后面没有分隔符）。
    quint32 textLength = 0;
};

const TableIndex &tableIndex()
{
    // 函数内静态变量：C++11 起初始化是线程安全的，且只跑一次
    static const TableIndex idx = [] {
        TableIndex out;
        out.start.reserve(pinyin::data::kEntryCount + 1);
        out.start.append(0);   // 第一项从偏移 0 开始
        quint32 pos = 0;
        while (pinyin::data::kTable[pos] != '\0') {
            if (pinyin::data::kTable[pos] == QLatin1Char(' ')) {
                ++pos;   // 跳过这个分隔符，它后面的字节就是下一项的开头
                out.start.append(pos);
                continue;   // 上面的 ++pos 已经把位置推过去了，别再推一次
            }
            ++pos;
        }
        out.textLength = pos;
        return out;
    }();
    return idx;
}

} // namespace

namespace pinyin {

QLatin1StringView syllable(QChar ch, bool alternateReading)
{
    const int cp = ch.unicode();
    if (cp < data::kFirstCodePoint || cp > data::kLastCodePoint)
        return {};   // 非汉字、CJK 扩展区、生僻字：交给调用方当分隔符处理

    const TableIndex &index = tableIndex();
    const int item = cp - data::kFirstCodePoint;
    if (item >= index.start.size())
        return {};   // 表项比码位少（表被改坏）：宁可查不到，也不要越界读

    const quint32 begin = index.start.at(item);
    // start[i+1] 指向下一项的起点，减 1 正好落在分隔符上；最后一项用表体总长收尾
    const quint32 end = (item + 1 < index.start.size()) ? index.start.at(item + 1) - 1
                                                       : index.textLength;
    if (end <= begin)
        return {};

    // 表项形如 "le" 或 "le|yue"（多音字：主读音|次读音）
    const QLatin1StringView readings(data::kTable + begin, end - begin);
    if (readings == QLatin1String("-"))
        return {};   // 生成时标记的「无读音」占位

    const qsizetype bar = readings.indexOf(QLatin1Char('|'));
    if (bar < 0)
        return readings;   // 单音字：次读音就是它自己，换成次读音的整串仍然完整可读
    return alternateReading ? readings.sliced(bar + 1) : readings.first(bar);
}

QString full(const QString &text, bool alternateReading)
{
    QString out;
    out.reserve(text.size() * 3);   // 汉字平均 3 个字母，够用且避免中途扩容

    for (const QChar ch : text) {
        if (isAsciiAlnum(ch)) {
            out.append(ch.toLower());
            continue;
        }
        const QLatin1StringView reading = syllable(ch, alternateReading);
        if (!reading.isEmpty())
            out.append(reading);
    }
    return out;
}

QString initials(const QString &text, bool alternateReading)
{
    QString out;
    bool inLatinRun = false;   ///< 上一个字符是否属于同一段连续拉丁字母/数字

    for (const QChar ch : text) {
        if (isAsciiAlnum(ch)) {
            // 连续的拉丁串只取第一个字符：Visual Studio Code → v..s..c，
            // 而 WPS Office 只贡献 w，整串 wps 由 full() 覆盖，两条路都能命中。
            if (!inLatinRun)
                out.append(ch.toLower());
            inLatinRun = true;
            continue;
        }

        inLatinRun = false;
        const QLatin1StringView reading = syllable(ch, alternateReading);
        if (!reading.isEmpty())
            out.append(reading.at(0));   // 汉字逐字取声母：微信读书 → wxds
    }
    return out;
}

} // namespace pinyin

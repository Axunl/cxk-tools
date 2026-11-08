#pragma once

// ---------------------------------------------------------------------------
// OCR 纯逻辑：词模型、后端输出解析、行/列还原、文本拼接。
//
// 为什么单独抽成 header：
//   * 规则 2 要求改动必须有实际跑过的验证，纯逻辑放进 header 才能在临时工程里
//     直接 include 做断言，不必把 QWidget 或 QProcess 拖进测试；
//   * Windows 系统 OCR 与 tesseract 的原始输出格式完全不同，统一在这里归一化成
//     同一套「带坐标的词」，上层（面板、平台后端）就不必各写一套排版逻辑。
//
// 约束：只依赖 QtCore 的值类型，不 include 任何 QWidget / 平台头。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>

namespace ocrtext {

/// 一个识别出来的「词」及其在原图（设备像素）里的包围盒。
/// Windows 后端给的是 OcrWord.BoundingRect，tesseract 给的是 TSV 的 left/top/width/height，
/// 两者坐标单位都是图像像素，因此可以直接共用同一套排版算法。
struct Word
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    QString text;
};

/// 输出形态。Table 只是「按空隙分列的 TSV 文本」，不是结构化表格识别：
/// 系统 OCR 不返回单元格，硬猜行列不如给一份能直接粘进表格软件的对齐文本。
enum class OutputMode
{
    Plain,   ///< 自然段文字
    Table    ///< 按词间空隙插入 Tab 的轻量表格文本
};

/// CJK 汉字、中文标点与全角字符。
/// WinRT OCR 会把「工具箱」切成「工」「具」「箱」三个词并默认以空格相连，
/// 拼接时必须靠字符类别判断要不要补空格（见 joinLine）。
inline bool isCjk(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x3400 && u <= 0x9FFF)      // 扩展 A + 基本汉字
        || (u >= 0xF900 && u <= 0xFAFF)      // 兼容汉字
        || (u >= 0x3000 && u <= 0x303F)      // CJK 标点（。、《》等）
        || (u >= 0xFF00 && u <= 0xFFEF);     // 全角标点／全角字母数字
}

/// 中文标点：紧跟内容时不该有空格（「工具 ，」→「工具，」）。
/// 英文标点不在此列，避免把 "Hello, world" 拼成 "Hello,world"。
inline bool isCjkPunct(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x3000 && u <= 0x303F) || (u >= 0xFF00 && u <= 0xFF65);
}

/// 解析 Windows 后端的行协议：W|x|y|w|h|text。
///
/// 文本本身可能含 '|'（表格竖线、代码里的或运算符），所以只切前 6 段，余下重新拼回。
/// 非 "W|" 开头的行一律忽略：PowerShell 的警告/进度输出不该混进识别结果。
/// 以 "__OCR_" 开头的行是脚本主动报告的失败标记，遇到即整体失败。
/// @return false 表示后端明确报告了错误，此时 error 为可读原因。
inline bool parseWindowsWords(const QString &output, QVector<Word> *words, QString *error)
{
    if (words)
        words->clear();
    if (error)
        error->clear();

    // PowerShell 5.1 把 Console.OutputEncoding 设为 UTF8 后，某些版本会在 stdout
    // 开头写 BOM；Qt 侧已按 UTF-8 解码，这里再兜掉残留的 U+FEFF。
    QString text = output;
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);

    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();   // 去掉 Windows 的 \r 与行首尾空白
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("__OCR_"))) {
            if (error)
                *error = line;
            return false;
        }
        if (!line.startsWith(QLatin1String("W|")))
            continue;

        const QStringList parts = line.split(QLatin1Char('|'));
        if (parts.size() < 6)
            continue;

        bool okX = false, okY = false, okW = false, okH = false;
        Word word;
        word.x = parts.at(1).toInt(&okX);
        word.y = parts.at(2).toInt(&okY);
        word.width = parts.at(3).toInt(&okW);
        word.height = parts.at(4).toInt(&okH);
        word.text = parts.mid(5).join(QLatin1Char('|'));
        if (!(okX && okY && okW && okH) || word.text.isEmpty())
            continue;   // 跳过格式不完整的行，不让一条脏数据毁掉整次识别
        if (words)
            words->append(word);
    }
    return true;
}

/// 解析 tesseract 的 TSV 输出（macOS / Linux 后端）。
///
/// 表头固定为 level..text 共 12 列；只取 level==5 的 word 行，其余（page/block/
/// paragraph/line 汇总行）没有词文本，取了会重复。
/// 列序：0 level, 1 page, 2 block, 3 par, 4 line, 5 word,
///       6 left, 7 top, 8 width, 9 height, 10 conf, 11 text。
inline bool parseTesseractTsv(const QString &output, QVector<Word> *words, QString *error)
{
    if (words)
        words->clear();
    if (error)
        error->clear();

    bool sawHeader = false;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("level\t"))) {
            sawHeader = true;
            continue;
        }
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 12 || fields.at(0) != QLatin1String("5"))
            continue;

        bool okX = false, okY = false, okW = false, okH = false;
        Word word;
        word.x = fields.at(6).toInt(&okX);
        word.y = fields.at(7).toInt(&okY);
        word.width = fields.at(8).toInt(&okW);
        word.height = fields.at(9).toInt(&okH);
        word.text = fields.at(11);
        if (!(okX && okY && okW && okH) || word.text.trimmed().isEmpty())
            continue;
        if (words)
            words->append(word);
    }

    // 一个词都没有又没见过表头，通常是 tesseract 报错把 stderr 当输出了，
    // 不能当成「图片里没文字」静默放过。
    if (!sawHeader && (!words || words->isEmpty())) {
        if (error)
            *error = QStringLiteral("tesseract 输出格式异常（没有 TSV 表头）");
        return false;
    }
    return true;
}

/// 聚类后的一行。y 取该行第一个词的 top，用作后续词的归行基准。
struct Line
{
    int y = 0;
    QVector<Word> words;
};

/// 按 y 坐标聚成行，行内再按 x 排序。
/// 同一行词的 y 常有 1–3px 抖动；中文逗号/句号的基线还会比同行汉字低约半个字高，
/// 所以容差取 3/4 行高并夹在 6..24 之间。不写死像素是因为截图可能来自 2x
/// 高 DPI 屏，固定值会把一行拆成两行。
inline QVector<Line> groupLines(const QVector<Word> &words)
{
    QVector<Line> lines;
    if (words.isEmpty())
        return lines;

    QVector<Word> sorted = words;
    std::sort(sorted.begin(), sorted.end(), [](const Word &a, const Word &b) {
        if (a.y != b.y)
            return a.y < b.y;
        return a.x < b.x;
    });

    for (const Word &word : sorted) {
        if (lines.isEmpty()) {
            Line line;
            line.y = word.y;
            line.words.append(word);
            lines.append(line);
            continue;
        }

        Line &current = lines.last();
        const int rowHeight = qMax(current.words.first().height, word.height);
        const int tolerance = qBound(6, rowHeight * 3 / 4, 24);
        if (qAbs(word.y - current.y) <= tolerance) {
            current.words.append(word);
        } else {
            Line line;
            line.y = word.y;
            line.words.append(word);
            lines.append(line);
        }
    }

    for (Line &line : lines) {
        std::sort(line.words.begin(), line.words.end(),
                  [](const Word &a, const Word &b) { return a.x < b.x; });
    }
    return lines;
}

/// 把一行词拼成文本。
///
/// 两条规则：
///  1. Table 模式下词间空隙大于一个字高就插入 Tab（阈值随字号缩放，天然适配高 DPI）；
///  2. 其余情况仅在「两边都不是中文」时补空格。WinRT OCR 把每个汉字切成独立词，
///     若一律补空格会得到「最 近 使 用」；而 "CXK 工具箱" 的英文与汉字之间
///     本来就该留空格，所以只有双方都是 CJK 才连写。中文标点一律紧贴前后内容。
inline QString joinLine(const QVector<Word> &lineWords, OutputMode mode)
{
    QString out;
    int prevRight = 0;
    bool first = true;

    for (const Word &word : lineWords) {
        if (!first) {
            const int gap = word.x - prevRight;
            const int rowHeight = qMax(word.height, 12);
            if (mode == OutputMode::Table && gap > qMax(12, rowHeight)) {
                out += QLatin1Char('\t');
            } else {
                const QChar before = out.isEmpty() ? QChar() : out.at(out.size() - 1);
                const QChar after = word.text.isEmpty() ? QChar() : word.text.at(0);
                const bool glue = (isCjk(before) && isCjk(after))
                    || (before.isNull() ? false : isCjkPunct(before))
                    || (after.isNull() ? false : isCjkPunct(after));
                if (!glue)
                    out += QLatin1Char(' ');
            }
        }
        out += word.text;
        // 用最右边界而不是当前词的右边界：重叠/嵌套的词不会让 gap 算成负数
        prevRight = qMax(prevRight, word.x + word.width);
        first = false;
    }
    return out;
}

/// 生成最终文本：行间换行、行尾空格清掉（Tab 保留，Table 模式靠它分列）。
inline QString render(const QVector<Word> &words, OutputMode mode)
{
    const QVector<Line> lines = groupLines(words);
    QStringList rows;
    rows.reserve(lines.size());
    for (const Line &line : lines) {
        QString row = joinLine(line.words, mode);
        while (row.endsWith(QLatin1Char(' ')))
            row.chop(1);
        rows.append(row);
    }
    return rows.join(QLatin1Char('\n'));
}

/// 统计非空白字符数，供面板状态栏显示「识别到 N 个字符」。
inline int countNonSpace(const QString &text)
{
    int count = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (!text.at(i).isSpace())
            ++count;
    }
    return count;
}

} // namespace ocrtext

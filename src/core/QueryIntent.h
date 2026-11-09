#pragma once

// ---------------------------------------------------------------------------
// 首页搜索的「意图识别」（纯逻辑，不依赖 QWidget，可单独写临时断言验证）。
//
// 解决的三个场景：
//   1. 用户往搜索框里粘了一整段 JSON —— 按名字搜不到任何工具，但显然想格式化它；
//   2. 用户往搜索框里粘了一张图片 —— QLineEdit 根本装不下图片，得走 OCR；
//   3. 用户输入一段英文但没有任何工具/应用命中 —— 多半是想翻译。
//
// 这一层只回答「这段输入像什么、该交给哪个工具」，不碰界面、不认识 ToolRegistry。
// 工具是否真的存在、提示长什么样，都由 HomePage 决定（插件缺失时就不提示）。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSize>
#include <QString>

namespace queryintent {

/// 建议交给哪个工具。工具 id 与内置插件约定一致（plugins/jsonfmt、plugins/ocr、plugins/translate）。
inline QString jsonToolId() { return QStringLiteral("jsonfmt"); }
inline QString ocrToolId() { return QStringLiteral("ocr"); }
inline QString translateToolId() { return QStringLiteral("translate"); }

enum class Kind {
    None,        ///< 没有建议，按原来的搜索结果展示
    Json,        ///< 输入是一段合法 JSON
    Image,       ///< 剪贴板里粘进来的图片
    Translate,   ///< 什么都没搜到，且输入是英文
};

/// 一条建议。title / detail 由 HomePage 组装（它知道工具的真实展示名）。
struct Suggestion
{
    Kind    kind = Kind::None;
    QString toolId;
    QString text;    ///< 要带给工具的文本（JSON / 英文原文）
    QSize   imageSize;

    bool valid() const { return kind != Kind::None; }
};

/// JSON 检测的长度上限。搜索框是单行输入框，正常不会有这么大的内容；
/// 设上限是为了避免「粘了 10 MB 文本」时每敲一个键都去做一次全量解析。
constexpr int kMaxJsonChars = 64 * 1024;

/// 这段输入是否是一段完整的 JSON（对象或数组）。
///
/// 只认 `{...}` 与 `[...]`：裸标量（`123`、`"abc"`、`true`）在 JSON 语法里也合法，
/// 但它们与普通搜索词无法区分，误判代价（把搜索变成格式化）比漏判大得多。
inline bool looksLikeJson(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.size() < 2 || trimmed.size() > kMaxJsonChars)
        return false;

    const QChar first = trimmed.at(0);
    if (first != QLatin1Char('{') && first != QLatin1Char('['))
        return false;

    // 结尾字符先做一次廉价检查：输入到一半（`{"a":`）时不至于每键都解析失败一遍
    const QChar last = trimmed.at(trimmed.size() - 1);
    if (last != QLatin1Char('}') && last != QLatin1Char(']'))
        return false;

    QJsonParseError error{};
    // JSON 规范就是 UTF-8，走 toUtf8 不会丢内容
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && !doc.isNull();
}

/// 这段输入是否「基本是英文」。
///
/// 判定：至少两个拉丁字母，且**不含**任何非拉丁文字。中文标点、数字、符号都不影响判定
/// （它们不是字母）。遇到代理对字符（emoji、生僻汉字）直接判否 —— 宁可不出翻译建议，
/// 也不要把一段中文当成英文送出去。
inline bool isEnglishText(const QString &text)
{
    int latinLetters = 0;
    for (const QChar ch : text) {
        if (ch.isHighSurrogate() || ch.isLowSurrogate())
            return false;
        if (!ch.isLetter())
            continue;
        if (ch.script() != QChar::Script_Latin)
            return false;
        ++latinLetters;
    }
    return latinLetters >= 2;
}

/// 把要展示的原文压成一行并截断，避免长内容把建议条撑成两行以上。
/// 真正显示时 IntentBanner 还会按控件宽度再做一次省略（这里只是兜底上限）。
inline QString preview(const QString &text, int maxChars = 200)
{
    QString flat = text.simplified();
    if (flat.size() > maxChars)
        flat = flat.left(maxChars - 1) + QChar(0x2026);   // …
    return flat;
}

///
/// 文本输入的建议。
/// @param hasToolHits 工具或本机应用是否已有命中 —— 有命中说明用户是在正常搜索，
///                    这时**不**该给翻译建议（否则每输一个英文工具名都会冒出翻译提示）。
///
inline Suggestion forText(const QString &text, bool hasToolHits)
{
    Suggestion s;
    if (looksLikeJson(text)) {
        s.kind = Kind::Json;
        s.toolId = jsonToolId();
        s.text = text.trimmed();
        return s;
    }

    if (!hasToolHits && isEnglishText(text)) {
        s.kind = Kind::Translate;
        s.toolId = translateToolId();
        s.text = text.trimmed();
    }
    return s;
}

/// 图片输入的建议（图片本身由 HomePage 持有，这里只带尺寸用于文案）。
inline Suggestion forImage(const QSize &size)
{
    Suggestion s;
    s.kind = Kind::Image;
    s.toolId = ocrToolId();
    s.imageSize = size;
    return s;
}

} // namespace queryintent

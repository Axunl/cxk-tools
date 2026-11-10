#pragma once

// ---------------------------------------------------------------------------
// 表情包搜索的「纯逻辑层」：URL 构造、两个数据源的响应解析、图片 URL 归一化、
// 缓存键生成、图片格式嗅探、标题清洗。
//
// 为什么单独拆成一个 header：AGENTS.md 规则 2 要求交付前有实际跑过的验证，
// 而仓库不保留测试工程。把这些逻辑做成不依赖 QWidget / 网络的 inline 函数后，
// 一个只链 Qt6::Core 的临时程序就能 include 本文件做断言，测完即删。
// 面板里的任何东西都不该进这里。
//
// 两条硬性约束（实现时踩过，别改回去）：
//   1. 发表情的搜索结果必须「按标记切块」解析。条目内部有嵌套 <div>，
//      用 `searchbqppdiv.*?</div>` 这类惰性整块匹配会在第一个 </div> 就截断，
//      结果只拿到图片地址、拿不到详情页链接。
//   2. 发表情在「关键词无结果」时不是返回空列表，而是回落成首页推荐流：
//      页面里只有 class="bqppdiv"、没有 class="searchbqppdiv"，data-original 一个都没有。
//      必须把这种情况判成「0 条」而不是「解析失败」，否则会一直误报结构异常。
// ---------------------------------------------------------------------------

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace meme {

/// 发表情搜索结果的固定主机名。取图必须带这个 Referer，否则图床直接 403。
inline constexpr auto kFabiaoqingBase = "https://www.fabiaoqing.com";
inline constexpr auto kFabiaoqingReferer = "https://www.fabiaoqing.com/";

/// 一条搜索结果。
/// thumbUrl 给网格用（小、省流量），fullUrl 给预览与另存为用。
/// 发表情两者相同（实测 bmiddle 与 large 返回完全一致的字节）；
/// 360 的 thumbUrl 是 240px 缩略图，fullUrl 才是原图。
struct MemeItem
{
    QString thumbUrl;
    QString fullUrl;
    QString detailUrl;   ///< 详情页；360 没有详情页，留空
    QString title;
    QString referer;     ///< 取图时要带的 Referer；空表示不带头
    QString source;      ///< "fabiaoqing" / "so360"，只用于状态栏文案
};

/// 嗅探出的图片格式。不信扩展名也不信 Content-Type —— 图床经常把 gif 用 .jpg 发。
enum class ImageKind
{
    Unknown,
    Gif,
    Jpeg,
    Png,
    Bmp,
    Webp,
};

// 解析函数在下面会用到这几个工具函数，但它们的定义排在后面（按「URL → 解析 →
// 归一化 → 文本」的顺序读起来更顺）。这里补前置声明，别把定义顺序也一起改乱。
inline QString normalizeImageUrl(const QString &raw);
inline QString cleanTitle(const QString &raw);
inline QString decodeEntities(const QString &raw);

// --------------------------------------------------------------- URL 构造

/// 发表情搜索页。page 从 1 开始，每页 15 条。
/// 参数用 QUrl::toPercentEncoding 编码：中文关键词必须百分号编码，
/// 直接塞进 URL 会被服务端当成乱码关键词而返回推荐流。
inline QString fabiaoqingSearchUrl(const QString &keyword, int page)
{
    const int safePage = page < 1 ? 1 : page;
    return QStringLiteral("%1/search/bqb/keyword/%2/type/bq/page/%3.html")
        .arg(QLatin1String(kFabiaoqingBase),
             QString::fromLatin1(QUrl::toPercentEncoding(keyword)),
             QString::number(safePage));
}

/// 360 图片的 JSON 接口。offset 从 0 开始，count 是一页要几条。
inline QString so360SearchUrl(const QString &keyword, int offset, int count)
{
    const int safeOffset = offset < 0 ? 0 : offset;
    const int safeCount = count < 1 ? 1 : count;
    return QStringLiteral("https://image.so.com/j?q=%1&src=srp&sn=%2&pn=%3")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(keyword)),
             QString::number(safeOffset),
             QString::number(safeCount));
}

// --------------------------------------------------------------- 解析

/// 从一段 HTML 片段里取某个属性的值。不依赖属性顺序，也不管它出现在哪个标签上。
inline QString attrValue(const QString &chunk, const char *name)
{
    // 自定义分隔符 RE 而不是默认的 )"：正则里本身就含 `*)"` 这样的序列，
    // 用默认分隔符会把它当成 raw string 的结束标记，编译期报「字符串中有换行符」。
    const QRegularExpression re(QStringLiteral(R"RE((?:^|[\s<])%1\s*=\s*"([^"]*)")RE")
                                    .arg(QLatin1String(name)));
    const QRegularExpressionMatch match = re.match(chunk);
    return match.hasMatch() ? match.captured(1) : QString();
}

namespace detail {

/// 定位所有结果块 `class="...searchbqppdiv..."` 的起始下标。
/// 这里只取「起点」而不做完整标签匹配，就是为了绕开嵌套 div 的截断问题。
inline QVector<int> blockStarts(const QString &text)
{
    static const QRegularExpression marker(
        QStringLiteral(R"(class="[^"]*searchbqppdiv[^"]*")"));
    QVector<int> starts;
    auto it = marker.globalMatch(text);
    while (it.hasNext())
        starts.append(it.next().capturedStart());
    return starts;
}

} // namespace detail

/// 解析发表情搜索结果页。
/// 返回值语义（面板按它决定下一步，别混用）：
///   > 0  解析到的条数
///   == 0 页面正常但没有结果（站点回落成推荐流）。面板应换源或提示换关键词。
///   < 0  页面结构不认识（改版、验证页、不是 HTML）。面板应换源并写明原因。
inline int parseFabiaoqingPage(const QByteArray &html, QVector<MemeItem> *out, QString *error)
{
    if (out)
        out->clear();

    const QString text = QString::fromUtf8(html);
    const QVector<int> starts = detail::blockStarts(text);

    if (starts.isEmpty()) {
        // 没有结果块有两种可能：无结果回落，或者页面根本不是搜索页。
        // 推荐流的条目用的是 class="bqppdiv"（不带 search 前缀），借它区分。
        if (text.contains(QStringLiteral("bqppdiv")))
            return 0;
        if (error)
            *error = QStringLiteral("页面结构可能已变化（没有找到结果块）");
        return -1;
    }

    for (int i = 0; i < starts.size(); ++i) {
        const int begin = starts.at(i);
        const int end = (i + 1 < starts.size()) ? starts.at(i + 1) : text.size();
        const QString chunk = text.mid(begin, end - begin);

        // 只认 data-original：src 是懒加载占位图（/Public/lazyload/img/transparent.gif），
        // 取它会得到一堆"透明像素"，而且下载回来嗅探不出任何图片格式。
        const QString img = attrValue(chunk, "data-original");
        if (img.isEmpty())
            continue;

        MemeItem item;
        item.thumbUrl = normalizeImageUrl(img);
        item.fullUrl = item.thumbUrl;
        if (item.thumbUrl.isEmpty())
            continue;

        const QString href = attrValue(chunk, "href");
        if (!href.isEmpty()) {
            item.detailUrl = href.startsWith(QLatin1String("http"))
                                 ? href
                                 : QLatin1String(kFabiaoqingBase) + href;
        }
        item.title = cleanTitle(attrValue(chunk, "title"));
        item.referer = QLatin1String(kFabiaoqingReferer);
        item.source = QStringLiteral("fabiaoqing");
        out->append(item);
    }

    if (out->isEmpty()) {
        if (error)
            *error = QStringLiteral("结果块解析不出图片地址（页面结构可能已变化）");
        return -1;
    }
    return out->size();
}

/// 解析 360 图片的 JSON 响应。语义同 parseFabiaoqingPage：>0 条数，0 无结果，<0 结构异常。
inline int parseSo360Json(const QByteArray &json, QVector<MemeItem> *out, QString *error)
{
    if (out)
        out->clear();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull()) {
        if (error)
            *error = QStringLiteral("返回的不是合法 JSON：%1").arg(parseError.errorString());
        return -1;
    }

    const QJsonValue listValue = doc.object().value(QStringLiteral("list"));
    if (!listValue.isArray()) {
        if (error)
            *error = QStringLiteral("返回里没有 list 字段（接口可能已变化）");
        return -1;
    }

    const QJsonArray list = listValue.toArray();
    for (const QJsonValue &value : list) {
        const QJsonObject object = value.toObject();

        // 优先拼 https 原图：img 字段是 http 且常常指向第三方站点（ws.126.net 之类），
        // 那些域名不一定有证书，无脑升级协议会从"能下载"变成"连不上"。
        const QString host = object.value(QStringLiteral("https")).toString();
        const QString key = object.value(QStringLiteral("imgkey")).toString();

        MemeItem item;
        item.thumbUrl = normalizeImageUrl(object.value(QStringLiteral("_thumb")).toString());
        item.fullUrl = (!host.isEmpty() && !key.isEmpty())
                           ? QStringLiteral("https://%1/%2").arg(host, key)
                           : normalizeImageUrl(object.value(QStringLiteral("img")).toString());
        if (item.fullUrl.isEmpty())
            continue;
        if (item.thumbUrl.isEmpty())
            item.thumbUrl = item.fullUrl;

        item.title = cleanTitle(object.value(QStringLiteral("title")).toString());
        item.source = QStringLiteral("so360");
        item.referer.clear();   // 360 的图床不校验 Referer
        out->append(item);
    }

    return out->size();
}

// --------------------------------------------------------------- 归一化

/// 把 HTML 里的属性值变成可直接请求的 URL。
/// 处理的三种情况：协议相对（//host/x）、HTML 实体（&amp;）、以及已知支持 TLS 的
/// 图床把 http 升到 https。其余 http 一律保留原协议 —— 见 parseSo360Json 里的说明。
inline QString normalizeImageUrl(const QString &raw)
{
    QString value = decodeEntities(raw).trimmed();
    if (value.isEmpty())
        return QString();

    if (value.startsWith(QLatin1String("//")))
        return QStringLiteral("https:") + value;

    QUrl url(value);
    if (!url.isValid() || url.host().isEmpty())
        return value;

    if (url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0) {
        // 只对实测支持 TLS 的图床升级协议；无条件升级会让没有证书的小站直接失败。
        static const QStringList tlsHosts = {
            QStringLiteral("soutula.com"),
            QStringLiteral("qhimg.com"),
            QStringLiteral("qhimgs1.com"),
            QStringLiteral("qhmsg.com"),
            QStringLiteral("so.com"),
        };
        const QString host = url.host().toLower();
        for (const QString &suffix : tlsHosts) {
            if (host == suffix || host.endsWith(QLatin1Char('.') + suffix)) {
                url.setScheme(QStringLiteral("https"));
                return url.toString();
            }
        }
    }
    return value;
}

/// 缓存文件名用的键：图片 URL 的 SHA-256。
/// 归一化掉 fragment（同一个图不同锚点不该占两份缓存）与大小写不同但等价的主机名。
inline QString cacheKey(const QString &url)
{
    QUrl parsed(url);
    parsed.setFragment(QString());
    const QByteArray encoded =
        parsed.toEncoded(QUrl::RemoveFragment | QUrl::RemoveUserInfo | QUrl::RemovePassword);
    return QString::fromLatin1(
        QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
}

// --------------------------------------------------------------- 格式嗅探

/// 按字节魔数判断格式。不用扩展名：发表情把 gif 挂在 .jpg 上并不罕见；
/// 也不用 Content-Type：有些图床一律回 application/octet-stream。
inline ImageKind sniffImageKind(const QByteArray &bytes)
{
    if (bytes.size() >= 6) {
        const QByteArray head = bytes.left(6);
        if (head.startsWith("GIF87a") || head.startsWith("GIF89a"))
            return ImageKind::Gif;
    }
    if (bytes.size() >= 3) {
        const uchar *p = reinterpret_cast<const uchar *>(bytes.constData());
        if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
            return ImageKind::Jpeg;
    }
    if (bytes.size() >= 8) {
        static const uchar pngMagic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        bool isPng = true;
        for (int i = 0; i < 8; ++i) {
            if (static_cast<uchar>(bytes.at(i)) != pngMagic[i]) {
                isPng = false;
                break;
            }
        }
        if (isPng)
            return ImageKind::Png;
    }
    if (bytes.size() >= 2 && bytes.startsWith("BM"))
        return ImageKind::Bmp;
    if (bytes.size() >= 12 && bytes.startsWith("RIFF")
        && bytes.mid(8, 4) == QByteArrayLiteral("WEBP")) {
        return ImageKind::Webp;
    }
    return ImageKind::Unknown;
}

inline QString extensionFor(ImageKind kind)
{
    switch (kind) {
    case ImageKind::Gif:
        return QStringLiteral("gif");
    case ImageKind::Jpeg:
        return QStringLiteral("jpg");
    case ImageKind::Png:
        return QStringLiteral("png");
    case ImageKind::Bmp:
        return QStringLiteral("bmp");
    case ImageKind::Webp:
        return QStringLiteral("webp");
    case ImageKind::Unknown:
        break;
    }
    return QString();
}

inline bool isAnimatedKind(ImageKind kind)
{
    return kind == ImageKind::Gif;
}

/// 运行时能不能解。WebP 返回 false：发布包里没有 qwebp 解码插件
/// （cmake/PruneQtRuntime.cmake 只保留 gif/jpeg/ico），解不了就必须明确报出来。
inline bool isSupportedKind(ImageKind kind)
{
    return kind == ImageKind::Gif || kind == ImageKind::Jpeg || kind == ImageKind::Png
           || kind == ImageKind::Bmp;
}

// --------------------------------------------------------------- 文本处理

/// 解 HTML 实体。发表情的 alt/title 里会出现 &amp; &#39; &quot;，
/// 不解就会把 "&amp;" 当成标题的一部分显示出来。
inline QString decodeEntities(const QString &raw)
{
    if (raw.isEmpty() || !raw.contains(QLatin1Char('&')))
        return raw;

    QString value = raw;
    value.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    value.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    value.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    value.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    value.replace(QStringLiteral("&#x27;"), QStringLiteral("'"));
    value.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    // &amp; 必须最后替换：否则 "&amp;lt;" 会先变成 "<"，再变成真标签。
    value.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return value;
}

/// 把开头连续重复的短单元压成一次。
/// 发表情很多标题就是表情上的字重复一堆（"无语无语无语…"、"你好烦你好烦…"），
/// 原样显示在 152px 的瓦片里全是同样的字，等于没有标题；直接截断又会截在半个词上，
/// 所以先做一次"去重"再交给 QLabel 省略。
inline QString collapseLeadingRepeats(const QString &text)
{
    // 单元最长 6 个字符：够覆盖「你好烦」「打工人」这类词，又不会把正常长句误判成重复。
    constexpr int kMaxUnit = 6;
    for (int unit = 1; unit <= kMaxUnit && unit * 3 <= text.size(); ++unit) {
        const QString piece = text.left(unit);
        int count = 1;
        while (text.mid(count * unit, unit) == piece)
            ++count;
        if (count < 3)
            continue;

        QString rest = text.mid(count * unit);
        // 末尾常常是被截断的同一单元（"无语无语无语无" 的最后一个"无"）：
        // 它不是新内容，留着只会显示成 "无语…无"，比不去重还怪。
        if (!rest.isEmpty() && rest.size() < unit && piece.startsWith(rest))
            rest.clear();
        return piece + QStringLiteral("…") + rest;
    }
    return text;
}

/// 清洗标题。
/// 发表情的 title 一律是「<表情文字> - <系列名>_<标签>_<标签>表情」，
/// 「 - 」后面是相册信息，对找图的人没有意义，直接截掉。
inline QString cleanTitle(const QString &raw)
{
    QString title = decodeEntities(raw).trimmed();
    const int dash = title.indexOf(QStringLiteral(" - "));
    if (dash > 0)
        title.truncate(dash);

    // 尾部挂着相册标签：「…_妈妈_开门_无语表情」。只有确实以「表情」结尾才截，
    // 免得误伤本身带下划线的正常标题。
    const int underscore = title.indexOf(QLatin1Char('_'));
    if (underscore > 0 && title.endsWith(QStringLiteral("表情")))
        title.truncate(underscore);

    // 连续空白（含全角空格与换行）压成一个半角空格，否则瓦片里会顶出一大段空白。
    static const QRegularExpression spaces(QStringLiteral("[\\s\\x{3000}]+"));
    title.replace(spaces, QStringLiteral(" "));
    return collapseLeadingRepeats(title.trimmed());
}

/// 1.5 KB 这种给人看的体积文案。
inline QString humanByteSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
    return QStringLiteral("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
}

} // namespace meme

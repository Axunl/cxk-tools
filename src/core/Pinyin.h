#pragma once

// ---------------------------------------------------------------------------
// 汉字 → 拼音（纯逻辑，不依赖 QWidget，可单独写临时断言验证）。
//
// 解决什么问题：首页搜索框只有一个，用户既可能打英文缩写（wx），也可能打全拼
// （weixin）或中文（微信）。应用/工具名大多是中文，光靠子串匹配时打 wx 一无所获，
// 所以这里提供「名称 → 拼音」的转换，让搜索层能拿缩写去比。
//
// 读音规则：
//   * 每个汉字有一个主读音，多音字额外带一个次读音（乐 le/yue、重 zhong/chong）。
//     主读音用来生成全拼与缩写；次读音生成「另一套」全拼与缩写，由搜索层各比一次，
//     这样 yinyue 也能搜到「网易云音乐」。两个以上读音不再枚举 —— 收益趋零，表却会膨胀；
//   * 拉丁字母与数字原样保留并转小写，其余字符（空格、标点、emoji）当分隔符；
//   * CJK 扩展区（U+3400 起）与生僻字不在表内：按分隔符处理，不影响同一名称里
//     其他字的匹配（例如「鑫」这类字只贡献一个词边界）。
// ---------------------------------------------------------------------------

#include <QLatin1StringView>
#include <QString>

namespace pinyin {

/// 单个汉字的无声调小写拼音（"微" → "wei"）。非汉字、表外字、无读音时返回空串。
/// @param alternateReading true 取次读音（多音字才有，单音字返回空）
QLatin1StringView syllable(QChar ch, bool alternateReading = false);

/// 全拼串：微信 → weixin，QQ 音乐 → qqyinyue，Visual Studio Code → visualstudiocode。
/// 分隔符（空格、标点等）被丢弃，因此可以拿「用户输入」直接比前缀。
QString full(const QString &text, bool alternateReading = false);

/// 首字母串：微信 → wx，微信读书 → wxds，Visual Studio Code → vsc。
/// 每个词取一个字母：汉字取声母，连续的拉丁字母/数字取第一个字符。
/// 用它可以实现「打 wx 找到微信」。
QString initials(const QString &text, bool alternateReading = false);

} // namespace pinyin

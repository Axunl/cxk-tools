#pragma once

// ---------------------------------------------------------------------------
// OCR 识别后端：插件里唯一碰「平台差异」的地方。
//
// 三端策略（保持离线、不引入新的编译依赖）：
//   Windows  —— 调用系统自带的 Windows.Media.Ocr（WinRT）。中文识别语言随系统
//               语言包提供，本机实测 zh-Hans-CN 可用，无需联网、无需打包模型。
//   macOS    —— 调用外部 tesseract 命令行（TSV 输出），未安装则给出明确提示。
//   Linux    —— 同 macOS。
// 两端最终都归一化成 ocrtext::Word（带坐标的词），排版逻辑全部复用 OcrText.h。
//
// 刻意不写 C++/WinRT：那需要 windowsapp.lib、winmd 投影头与异步初始化，
// 会把三端构建拖进平台专属依赖；用系统自带 PowerShell 调 WinRT 是等价且
// 零编译依赖的做法。macOS 的 Vision / Linux 的原生 OCR 没有实机验证环境，
// 按规则 3 留外部引擎路径并给出可读文案，不塞未经验证的原生代码。
// ---------------------------------------------------------------------------

#include "OcrText.h"

#include <QString>
#include <QVector>

namespace ocrengine {

/// 一次识别的结果。error 非空即失败（面板必须把原因显示出来，不允许静默）。
/// words 为空且 error 为空表示「识别成功但图里没有文字」，属正常结果。
struct Result
{
    QVector<ocrtext::Word> words;
    QString error;
    QString backend;   ///< 引擎名，供状态栏展示
};

/// 当前平台使用的引擎说明（面板初始状态用）。
QString backendName();

/// 同步识别一张图片文件（PNG/JPG 等，交给后端解码）。
///
/// 这是阻塞调用：最坏情况要等 PowerShell 冷启动 + WinRT 初始化（数秒）。
/// 之所以不做异步 QProcess：宿主随时会销毁工具面板（面板不缓存），
/// 异步回调一旦落到已析构的面板就是悬空指针；同步调用期间面板必然存活，
/// 面板只需在调用前刷新状态栏 + WaitCursor 即可，见 OcrPanel::recognizePixmap()。
/// @param imagePath 已落盘的临时图片路径，调用方负责生命周期
Result recognize(const QString &imagePath);

} // namespace ocrengine

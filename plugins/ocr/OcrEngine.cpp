#include "OcrEngine.h"

// ---------------------------------------------------------------------------
// 识别后端实现。平台分支固定四段：Windows / macOS / Linux / 其它兜底。
// 本文件不 include 任何 windows.h / AppKit / X11 头 —— 平台差异全部体现为
// 「调用哪个外部命令、解析哪种输出」，因此三端都能用同一套 Qt 代码编译。
//
// 三端统一的首选后端是 PP-OCR（ONNX Runtime，见 runOnnxOcr）；当 onnxruntime/
// 模型缺失或识别失败时，自动回退到下面的平台原生后端（WinRT / tesseract）。
// ---------------------------------------------------------------------------

#include "OcrOnnx.h"
#include "OcrPostprocess.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QtGlobal>

namespace ocrengine {
namespace {

/// PowerShell 找不到命令时的统一文案，避免三处重复。
QString processStartError(const QString &exe)
{
    return QStringLiteral("无法启动 OCR 引擎进程（%1）：请检查系统是否允许运行该程序")
        .arg(exe);
}

// ---------------------------------------------------------------------------
// PP-OCR（ONNX Runtime）后端：三端统一的首选识别路径。
//
// 管线：QImage 读图 → det（DBNet 出概率图）→ 阈值/连通域/外接矩形（OcrPostprocess）
//       → 逐框 crop → rec（SVTR_LCNet 出 CTC）→ ctcDecode → 组装 ocrtext::Word。
// 每条 det 框对应一个 Word（text=整行文字），坐标取框的外接矩形，完全复用
// OcrText.h 的 groupLines/joinLine 排版。
//
// 为什么这个后端是平台无关的：它只依赖 onnxruntime 的 C++ API（本身跨平台）
// 与 Qt 的 QImage，不碰任何 WinRT / AppKit / Xlib，因此三端编译一致。
// 引擎内部是同步阻塞调用，与 OcrEngine::recognize 的既定架构一致。
// ---------------------------------------------------------------------------
Result runOnnxOcr(const QString &imagePath)
{
    Result result;
    result.backend = QStringLiteral("PP-OCR（本地模型）");

    // onnxruntime 或模型缺失时直接给可读原因，由上层 recognize() 决定是否回退。
    if (!OcrOnnxEngine::isAvailable()) {
        result.error = QStringLiteral("__FALLBACK__");
        return result;
    }

    QImage image(imagePath);
    if (image.isNull()) {
        result.error = QStringLiteral("__FALLBACK__");   // 读图失败也回退旧后端
        return result;
    }

    OcrOnnxEngine engine;
    QString loadError;
    if (!engine.load(&loadError)) {
        result.error = QStringLiteral("__FALLBACK__");
        return result;
    }

    // det 输入长边限制 960，等比缩放（PaddleOCR det_limit_type=max）。缩放后
    // det 框坐标在「缩放图」坐标系，最后再映射回原图（乘缩放比例）。
    QImage scaledImage = image;
    const int maxSide = qMax(image.width(), image.height());
    if (maxSide > 960) {
        const qreal r = qreal(960) / qreal(maxSide);
        scaledImage = image.scaled(qMax(1, int(image.width() * r)),
                                   qMax(1, int(image.height() * r)),
                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    const qreal coordScaleX = image.width() > 0
        ? qreal(image.width()) / qreal(scaledImage.width()) : 1.0;
    const qreal coordScaleY = image.height() > 0
        ? qreal(image.height()) / qreal(scaledImage.height()) : 1.0;

    // det 前向（内部 pad 到 32 倍数）
    QString detError;
    const DetMap map = engine.runDet(scaledImage, &detError);
    if (!map.valid()) {
        result.error = QStringLiteral("__FALLBACK__");
        return result;
    }

    // det 后处理：概率图 → 文字框（坐标在缩放图坐标系）
    const QVector<ocrpost::DetBox> boxes = ocrpost::detBoxes(
        map.data, map.mapW, map.mapH, map.origW, map.origH);

    if (boxes.isEmpty())
        return result;   // 识别成功但图里没文字，属正常结果（error 空）

    const QStringList keys = engine.keys();

    // 逐框 crop → rec → CTC decode → 组装 Word
    for (const ocrpost::DetBox &box : boxes) {
        const int w = box.width();
        const int h = box.height();
        if (w <= 0 || h <= 0)
            continue;
        // crop 时夹紧到缩放图内，防止 det 框越界
        const QRect rect(qBound(0, box.x0, scaledImage.width() - 1),
                         qBound(0, box.y0, scaledImage.height() - 1),
                         qMin(w, scaledImage.width() - qBound(0, box.x0, scaledImage.width() - 1)),
                         qMin(h, scaledImage.height() - qBound(0, box.y0, scaledImage.height() - 1)));
        const QImage crop = scaledImage.copy(rect);
        if (crop.isNull())
            continue;

        QString recError;
        const RecProb prob = engine.runRec(crop, &recError);
        if (!prob.valid())
            continue;   // 单框识别失败不整条失败，跳过该框继续

        const QString text = ocrpost::ctcDecode(prob.data, prob.timesteps,
                                                prob.vocabSize, keys);
        if (text.trimmed().isEmpty())
            continue;

        ocrtext::Word word;
        // 框坐标从缩放图映射回原图（原图坐标系，与 WinRT/tesseract 后端一致）
        word.x = int(box.x0 * coordScaleX);
        word.y = int(box.y0 * coordScaleY);
        word.width = qMax(1, int(box.width() * coordScaleX));
        word.height = qMax(1, int(box.height() * coordScaleY));
        word.text = text;
        result.words.append(word);
    }

    return result;
}

#if defined(Q_OS_WIN)

// ---------------------------------------------------------------------------
// Windows：Windows.Media.Ocr（WinRT）调用脚本。
//
// 为什么输出「词级坐标」而不是整行文本：WinRT 会把每个汉字切成独立词并用空格
// 相连（「最近使用」→「最 近 使 用」），只拿 result.Text 没法还原；拿到
// BoundingRect 后由 OcrText.h 按字符类别决定连写还是补空格，还能顺带做表格分列。
//
// 脚本必须保持**纯 ASCII**（连注释也不能有中文）：PowerShell 5.1 读取无 BOM
// 的 .ps1 会按本地代码页（中文 Windows 是 GBK）解析，实测中文注释会让后面的
// WinRT 类型解析成 null，报「不能对 Null 值表达式调用方法」而整个识别失败。
// 这与项目规则 5 里 .bat 只能纯 ASCII 是同一个坑。
//
// 识别结果**不能走 stdout**：PS 5.1 在 stdout 被重定向为管道（QProcess 就是这么
// 干的）时，[Console]::OutputEncoding 不会生效，中文会按系统 ANSI 代码页（GBK）
// 输出，UTF-8 三字节的汉字被 GBK 按两字节重新切分、第三字节丢失，C++ 端
// fromUtf8 就得到「宸? 绠?」这类乱码。实测「写文件」方案完全正常：改用
// [System.IO.File]::WriteAllLines 直接写 UTF-8 无 BOM 文件，绕过控制台代码页。
// 结果文件路径与图片路径一样经环境变量传入（CXK_OCR_OUT），避免进入命令行。
// stdout 只用于纯 ASCII 的失败标记（__OCR_NO_*），这些不受编码影响。
// ---------------------------------------------------------------------------
const char *const kWinRtScript = R"OCRPS(
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

# PowerShell 5.1 has no await for WinRT; use AsTask().Wait() instead.
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]
function Await($WinRtTask, $ResultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtTask))
    try {
        $netTask.Wait(-1) | Out-Null
    } catch [System.AggregateException] {
        # Unwrap the AggregateException so the real WinRT failure (file-not-found,
        # access-denied, unsupported pixel format, oversized image, ...) surfaces
        # on stderr instead of a useless "one or more errors occurred".
        $inner = $_.Exception.InnerException
        if ($null -ne $inner) {
            Write-Error ("OCR-AWAIT-FAILED: " + $inner.GetType().FullName + " :: " + $inner.Message)
        } else {
            Write-Error ("OCR-AWAIT-FAILED: " + $_.Exception.ToString())
        }
        exit 5
    }
    $netTask.Result
}

$imagePath = $env:CXK_OCR_IMAGE
$outPath = $env:CXK_OCR_OUT
if (-not $imagePath) { Write-Output '__OCR_NO_IMAGE__'; exit 3 }
if (-not $outPath) { Write-Output '__OCR_NO_OUT__'; exit 4 }

# WinRT types need the full ContentType=WindowsRuntime syntax; short names fail in PS 5.1.
$decoderT = [Windows.Graphics.Imaging.BitmapDecoder,Windows.Foundation,ContentType=WindowsRuntime]
$softwareBitmapT = [Windows.Graphics.Imaging.SoftwareBitmap,Windows.Foundation,ContentType=WindowsRuntime]
$pixelFormatT = [Windows.Graphics.Imaging.BitmapPixelFormat,Windows.Foundation,ContentType=WindowsRuntime]
$engineT = [Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
$ocrResultT = [Windows.Media.Ocr.OcrResult,Windows.Foundation,ContentType=WindowsRuntime]

# Read the image with .NET file I/O instead of StorageFile.GetFileFromPathAsync.
# GetFileFromPathAsync is the flakiest link: it can fail with a vague
# "one or more errors occurred" if the just-written temp file is briefly locked
# by antivirus/another handle, and it does not accept all path forms. ReadAllBytes
# is synchronous and predictable; AsRandomAccessStream bridges the .NET bytes to
# the WinRT stream BitmapDecoder expects, avoiding DataWriter/StoreAsync (whose
# COM interop is unreliable in PS 5.1: "operation identifier is not valid").
$bytes = [System.IO.File]::ReadAllBytes($imagePath)
$ms = New-Object System.IO.MemoryStream(,$bytes)
$stream = [System.IO.WindowsRuntimeStreamExtensions]::AsRandomAccessStream($ms)

$decoder = Await ($decoderT::CreateAsync($stream)) $decoderT
$bitmap = Await ($decoder.GetSoftwareBitmapAsync()) $softwareBitmapT

# OCR accepts Bgra8 only; PNG decoding may yield Rgba8, so convert it.
$bgra8 = [Enum]::Parse($pixelFormatT,'Bgra8')
if ($bitmap.BitmapPixelFormat -ne $bgra8) {
    $bitmap = $softwareBitmapT::Convert($bitmap, $bgra8)
}

$engine = $engineT::TryCreateFromUserProfileLanguages()
if ($null -eq $engine) { Write-Output '__OCR_NO_ENGINE__'; exit 2 }
$result = Await ($engine.RecognizeAsync($bitmap)) $ocrResultT

# Write results to a UTF-8 (no BOM) file instead of stdout: PS 5.1 re-encodes
# piped stdout with the system ANSI codepage (GBK), which mangles Chinese by
# dropping the 3rd byte of each UTF-8 sequence. File I/O is codepage-safe.
$lines = @()
foreach ($line in $result.Lines) {
    foreach ($word in $line.Words) {
        $r = $word.BoundingRect
        $lines += ("W|{0}|{1}|{2}|{3}|{4}" -f [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height, $word.Text)
    }
}
[System.IO.File]::WriteAllLines($outPath, $lines, (New-Object System.Text.UTF8Encoding($false)))
)OCRPS";

/// 把脚本主动报告的失败标记翻译成用户能看懂的中文原因。
QString explainScriptMarker(const QString &marker)
{
    if (marker == QLatin1String("__OCR_NO_ENGINE__"))
        return QStringLiteral("系统没有可用的 OCR 识别语言：请在「Windows 设置 → 时间和语言 → 语言和区域」"
                              "安装中文（简体）语言包及可选的手写/基本输入组件");
    if (marker == QLatin1String("__OCR_NO_IMAGE__"))
        return QStringLiteral("OCR 引擎没有收到待识别的图片路径");
    if (marker == QLatin1String("__OCR_NO_OUT__"))
        return QStringLiteral("OCR 引擎没有收到结果输出路径");
    return marker;
}

/// 调用系统 OCR。返回词列表；失败时 result.error 必非空。
Result runWindowsOcr(const QString &imagePath)
{
    Result result;
    result.backend = QStringLiteral("Windows 系统 OCR");

    // 防呆：以后有人给脚本加中文注释时，PowerShell 5.1 会按 GBK 解析并静默
    // 弄坏 WinRT 类型解析。这里提前挡下并给明确原因，别让使用者对着
    // 「不能对 Null 值表达式调用方法」这种报错猜半天。
    const QByteArray scriptBytes(kWinRtScript);
    for (const char ch : scriptBytes) {
        if (static_cast<unsigned char>(ch) > 0x7F) {
            result.error = QStringLiteral("内置 OCR 脚本含非 ASCII 字符，PowerShell 5.1 无法正确解析"
                                          "（脚本注释必须保持纯 ASCII）");
            return result;
        }
    }

    QString exe = QStandardPaths::findExecutable(QStringLiteral("powershell"));
    if (exe.isEmpty()) {
        // 精简过 PATH 的环境里 System32 可能不在搜索路径中，显式退回系统目录。
        // 逐段用 QDir::filePath 拼，不写死反斜杠，也不假设大小写。
        const QDir systemRoot(qEnvironmentVariable("SystemRoot"));
        const QString psDir = QDir(QDir(systemRoot.filePath(QStringLiteral("System32")))
                                       .filePath(QStringLiteral("WindowsPowerShell")))
                                   .filePath(QStringLiteral("v1.0"));
        exe = QStandardPaths::findExecutable(QStringLiteral("powershell"),
                                             QStringList{psDir});
    }
    if (exe.isEmpty()) {
        result.error = QStringLiteral("找不到 Windows PowerShell，无法调用系统内置 OCR 引擎");
        return result;
    }

    // 用临时目录 + 普通 QFile 写 .ps1，而不是 QTemporaryFile：
    // QTemporaryFile 在 Windows 上会一直持有底层句柄，即便 close() 之后外部进程
    // 仍可能以「文件正由另一进程使用」打不开（实测 PowerShell -File 直接失败）。
    // QFile 写完 close() 就完全释放；QTemporaryDir 析构时连脚本一起删掉。
    // 也不用 -EncodedCommand：明文脚本便于审计，还能避开超长命令行与引号转义。
    QTemporaryDir scriptDir;
    if (!scriptDir.isValid()) {
        result.error = QStringLiteral("无法创建临时目录（系统临时目录不可写）");
        return result;
    }
    const QString scriptPath = QDir(scriptDir.path()).filePath(QStringLiteral("cxk-ocr.ps1"));
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.error = QStringLiteral("无法创建临时 OCR 脚本文件");
        return result;
    }
    (void)scriptFile.write(kWinRtScript);
    scriptFile.close();

    // 结果文件与脚本同放一个临时目录：QTemporaryDir 析构时一起删掉，不留垃圾。
    // 之所以不读 stdout 而读文件，见脚本上方注释（PS 5.1 管道中文乱码问题）。
    const QString outPath = QDir(scriptDir.path()).filePath(QStringLiteral("cxk-ocr-out.txt"));

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("CXK_OCR_IMAGE"), imagePath);
    env.insert(QStringLiteral("CXK_OCR_OUT"), outPath);

    QProcess process;
    process.setProcessEnvironment(env);
    process.start(exe, QStringList{
                           QStringLiteral("-NoProfile"),
                           QStringLiteral("-NonInteractive"),
                           QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                           QStringLiteral("-File"), scriptPath});
    if (!process.waitForStarted(5000)) {
        result.error = processStartError(exe);
        return result;
    }

    // PowerShell 冷启动 + Add-Type + WinRT 首次初始化可能要几秒，25s 足够；
    // 超时必须 kill，否则面板会一直卡在「识别中」。
    if (!process.waitForFinished(25000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = QStringLiteral("系统 OCR 识别超时（25 秒），已中止；可换一张小一点的图重试");
        return result;
    }

    // stdout 只承载纯 ASCII 的失败标记（__OCR_NO_*），按 UTF-8 解码即可；
    // 识别结果在 outPath 文件里，用 fromUtf8 读出，中文不会再被控制台代码页破坏。
    const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    // stderr 走的是系统 OEM 代码页（中文 Windows 为 GBK），用 fromLocal8Bit 才不乱。
    const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();

    if (process.exitStatus() == QProcess::CrashExit || process.exitCode() != 0) {
        const QString code = process.exitStatus() == QProcess::NormalExit
            ? QString::number(process.exitCode()) : QStringLiteral("进程异常退出");
        result.error = QStringLiteral("系统 OCR 调用失败（%1）%2")
                           .arg(code, stderrText.isEmpty()
                                         ? QString()
                                         : QStringLiteral("：") + stderrText.left(240));
        return result;
    }

    QFile outFile(outPath);
    if (!outFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("OCR 引擎没有生成结果文件（临时目录不可读）");
        return result;
    }
    const QString outText = QString::fromUtf8(outFile.readAll());
    outFile.close();

    QString parseError;
    if (!ocrtext::parseWindowsWords(outText, &result.words, &parseError)) {
        result.error = explainScriptMarker(parseError);
        return result;
    }
    return result;
}

#endif // Q_OS_WIN

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)

// ---------------------------------------------------------------------------
// macOS / Linux：外部 tesseract 命令行。
//
// 未实机验证过的平台（本机只有 Windows）：这两段只调用一个跨平台 CLI，
// 不含任何 AppKit / Xlib 代码，行为可解释；按规则 3 不塞未经验证的原生实现。
// tesseract 的 TSV 恰好提供词级坐标，与 Windows 后端共用 OcrText.h 排版逻辑。
// ---------------------------------------------------------------------------
Result runTesseract(const QString &imagePath, int attempt)
{
    Result result;
    result.backend = QStringLiteral("Tesseract");

    const QString exe = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
    if (exe.isEmpty()) {
        result.error = QStringLiteral("未检测到 Tesseract OCR 引擎：请先安装 tesseract-ocr"
                                      "（识别中文另需 chi_sim 语言包），再重新识别");
        return result;
    }

    // attempt 0 优先带中文语言包；失败降级到引擎默认语言再试一次。
    // 只装了英文包的机器上指定 eng+chi_sim 会直接失败，不能把「没装中文包」
    // 误报成「OCR 完全不可用」。--psm 6 假设一个统一文本块，适合截图取字。
    QStringList args{imagePath, QStringLiteral("stdout")};
    if (attempt == 0)
        args << QStringLiteral("-l") << QStringLiteral("eng+chi_sim");
    args << QStringLiteral("--psm") << QStringLiteral("6") << QStringLiteral("tsv");

    QProcess process;
    process.start(exe, args);
    if (!process.waitForStarted(5000)) {
        result.error = processStartError(exe);
        return result;
    }
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = QStringLiteral("Tesseract 识别超时（30 秒），已中止");
        return result;
    }

    const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (process.exitStatus() == QProcess::CrashExit || process.exitCode() != 0) {
        if (attempt == 0)
            return runTesseract(imagePath, 1);   // 语言包不可用 → 用默认语言重试
        result.error = QStringLiteral("Tesseract 识别失败%1")
                           .arg(stderrText.isEmpty()
                                    ? QString()
                                    : QStringLiteral("：") + stderrText.left(240));
        return result;
    }

    QString parseError;
    if (!ocrtext::parseTesseractTsv(stdoutText, &result.words, &parseError)) {
        result.error = parseError.isEmpty()
            ? QStringLiteral("无法解析 Tesseract 的输出") : parseError;
        return result;
    }
    return result;
}

#endif // Q_OS_MACOS || Q_OS_LINUX

} // namespace

QString backendName()
{
#if defined(XTOOLS_HAVE_ONNX)
    // 有 onnxruntime 且模型就绪时，首选 PP-OCR
    if (OcrOnnxEngine::isAvailable())
        return QStringLiteral("PP-OCR（本地模型，离线）");
#endif
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows 系统 OCR（离线，开箱可用）");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("Tesseract（需自行安装；识别中文请装 chi_sim 语言包）");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Tesseract（需自行安装 tesseract-ocr 与 chi_sim 语言包）");
#else
    return QStringLiteral("当前平台没有可用的 OCR 后端");
#endif
}

Result recognize(const QString &imagePath)
{
    Result result;

#if defined(XTOOLS_HAVE_ONNX)
    // 首选 PP-OCR（本地模型）：准确率最高。runOnnxOcr 在「模型缺失/加载失败/
    // 推理失败」时返回 error == "__FALLBACK__"，此时落回平台原生后端。
    // 这样即使 onnxruntime.dll 没随包分发、或模型损坏，识别也不会整体失效。
    if (OcrOnnxEngine::isAvailable()) {
        result = runOnnxOcr(imagePath);
        if (result.error != QLatin1String("__FALLBACK__"))
            return result;   // 成功（含「图里没文字」），或真正的错误 → 直接返回
        result.error.clear();   // 回退：清掉占位错误，走原生后端
    }
#endif

#if defined(Q_OS_WIN)
    result = runWindowsOcr(imagePath);
#elif defined(Q_OS_MACOS)
    // macOS 没有实机验证环境，走外部 Tesseract；缺引擎时返回可读原因（绝不静默）。
    result = runTesseract(imagePath, 0);
#elif defined(Q_OS_LINUX)
    result = runTesseract(imagePath, 0);
#else
    result.error = QStringLiteral("当前平台不支持 OCR 文字识别");
#endif

    return result;
}

} // namespace ocrengine

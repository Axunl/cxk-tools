#include "OcrOnnx.h"

// ---------------------------------------------------------------------------
// ONNX Runtime（PP-OCRv4）推理实现。本文件是插件里唯一 include onnxruntime 头
// 文件的地方；onnxruntime 的 C++ API（Ort::）全部藏在 PIMPL 的 Impl 里，不让
// 它扩散到头文件，保证 OcrOnnx.h 对调用方是「纯 Qt 值类型」的接口。
//
// 图像预处理约定（与 PaddleOCR 官方一致，实测 dump 确认）：
//   * 归一化：(px/255 - 0.5) / 0.5  → 值域 [-1, 1]
//   * 布局：NCHW（1 张图，RGB 三通道，H×W），float32
//   * det：长边限制 960（det_limit_side_len），超过则等比缩放；输入动态 shape
//   * rec：高固定 48，宽按长宽比缩放后对齐到 8 的倍数（模型 stride 8）
//
// 为什么 det/rec 共用一个 Session 对象（而非每识别一次新建）：
//   Ort::Session 的构造要读模型文件 + 图优化，开销约百毫秒级；面板一次截图可能
//   有几十个 det 框，逐个新建 Session 会慢到不可接受。load() 只建一次，runDet/
//   runRec 复用，符合「同步但尽量快」的既有架构约束。
// ---------------------------------------------------------------------------

#include "OcrPostprocess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPainter>

#include <cmath>

// onnxruntime 头文件只在编译期需要；未定义 XTOOLS_HAVE_ONNX 时整个文件退化成
// 「桩实现」，不 include onnxruntime，保证无 onnxruntime 的环境也能编译。
#if defined(XTOOLS_HAVE_ONNX)

#include <onnxruntime_cxx_api.h>

#include <QStandardPaths>

namespace ocrengine {
namespace {

// PP-OCR 固定参数（阶段 0 dump + PaddleOCR 官方约定）
constexpr int kDetLimitSideLen = 960;   // det 长边限制
constexpr int kDetStride = 32;          // det 输入必须对齐到 32 的倍数（实测：不对齐会
                                        // 在 Add 节点广播失败「25 by 26」）
constexpr int kRecHeight = 48;          // rec 固定输入高
constexpr int kRecAlign = 8;            // rec 宽对齐到 8 的倍数（模型 stride）

/// QImage（假定 RGB888）→ NCHW float 归一化张量数据。
/// 返回 c*h*w 连续内存的 float 数组；n=1（单张）。
QVector<float> imageToNchw(const QImage &img, int *outW, int *outH)
{
    const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    const int w = rgb.width();
    const int h = rgb.height();
    QVector<float> data(w * h * 3);

    // 按通道重排：data[c][y][x]，PaddleOCR 是 RGB 顺序（R 在前）
    // 逐行扫描，避免每像素做三次分支。
    int idx = 0;
    for (int c = 0; c < 3; ++c) {
        for (int y = 0; y < h; ++y) {
            const uchar *line = rgb.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                const uchar *px = line + x * 3;
                // (px/255 - 0.5) / 0.5 = px/127.5 - 1.0
                data[idx++] = float(px[c]) / 127.5f - 1.0f;
            }
        }
    }
    if (outW) *outW = w;
    if (outH) *outH = h;
    return data;
}

} // namespace

struct OcrOnnxEngine::Impl
{
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "cxk-ocr"};
    Ort::SessionOptions detOpts;
    Ort::SessionOptions recOpts;
    std::unique_ptr<Ort::Session> det;
    std::unique_ptr<Ort::Session> rec;
    QStringList keys;
    bool ok = false;
};

OcrOnnxEngine::OcrOnnxEngine() : d(new Impl) {}
OcrOnnxEngine::~OcrOnnxEngine() { delete d; }

bool OcrOnnxEngine::isAvailable()
{
    // 模型目录里有 det + rec + keys 才算可用；onnxruntime.dll 已随程序分发。
    const QString dir = modelDir();
    const QDir d(dir);
    if (!d.exists())
        return false;
    return d.exists(QStringLiteral("ch_PP-OCRv4_det_infer.onnx"))
        && d.exists(QStringLiteral("ch_PP-OCRv4_rec_infer.onnx"))
        && d.exists(QStringLiteral("ppocr_keys_v1.txt"));
}

QString OcrOnnxEngine::modelDir()
{
    // 与宿主的 PluginManager 约定一致：可执行文件同级的 plugins/ocr/models。
    // 用 QDir::filePath 拼，不写死反斜杠。
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("plugins/ocr/models"));
}

bool OcrOnnxEngine::load(QString *error)
{
    if (error) error->clear();

    const QString dir = modelDir();
    const QString detPath = QDir(dir).filePath(QStringLiteral("ch_PP-OCRv4_det_infer.onnx"));
    const QString recPath = QDir(dir).filePath(QStringLiteral("ch_PP-OCRv4_rec_infer.onnx"));
    const QString keysPath = QDir(dir).filePath(QStringLiteral("ppocr_keys_v1.txt"));

    // keys 字符表：UTF-8 逐行读，第 i 行 = 类索引 i+1
    QFile kf(keysPath);
    if (!kf.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取 OCR 字符表（ppocr_keys_v1.txt）");
        return false;
    }
    QStringList loadedKeys;
    const QByteArray raw = kf.readAll();
    kf.close();
    const QStringList lines = QString::fromUtf8(raw).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        QString s = line;
        if (s.endsWith(QLatin1Char('\r')))
            s.chop(1);
        loadedKeys.append(s);
    }
    if (loadedKeys.isEmpty()) {
        if (error) *error = QStringLiteral("OCR 字符表为空");
        return false;
    }

    try {
        // 线程数：CPU 推理，用硬件并发数，但不必超过 4（桌面截图图小，多线程收益递减）
        d->detOpts.SetIntraOpNumThreads(2);
        d->recOpts.SetIntraOpNumThreads(2);
        // Ort::Session 在 Windows 上只接受 wchar_t* 路径（宽字符），用 QString 转。
        const std::wstring detPathW = detPath.toStdWString();
        const std::wstring recPathW = recPath.toStdWString();
        d->det = std::make_unique<Ort::Session>(
            d->env, detPathW.c_str(), d->detOpts);
        d->rec = std::make_unique<Ort::Session>(
            d->env, recPathW.c_str(), d->recOpts);
    } catch (const Ort::Exception &e) {
        if (error) *error = QStringLiteral("OCR 模型加载失败：%1")
                                .arg(QString::fromUtf8(e.what()));
        d->det.reset();
        d->rec.reset();
        return false;
    }

    d->keys = loadedKeys;
    d->ok = true;
    return true;
}

bool OcrOnnxEngine::loaded() const { return d && d->ok; }

DetMap OcrOnnxEngine::runDet(const QImage &img, QString *error)
{
    DetMap out;
    if (error) error->clear();
    if (!loaded()) {
        if (error) *error = QStringLiteral("OCR 引擎未加载");
        return out;
    }

    // det 输入约定：调用方传入的 img 已按「长边 ≤ 960」缩放过。这里只负责
    // pad 到 32 的倍数（kDetStride，实测不对齐会在 Add 节点广播失败）再前向。
    // 记录 pad 前的尺寸（即传入 img 的尺寸），detBoxes 用它把概率图坐标映射回
    // 这个尺寸的坐标系。
    out.origW = img.width();
    out.origH = img.height();

    QImage scaled = img;
    // pad 到 32 的倍数：白底填充（文字检测对白底 pad 无感，右侧/下方补白即可）。
    const int padW = (img.width() + kDetStride - 1) / kDetStride * kDetStride;
    const int padH = (img.height() + kDetStride - 1) / kDetStride * kDetStride;
    if (padW != img.width() || padH != img.height()) {
        QImage padded(padW, padH, QImage::Format_RGB888);
        padded.fill(Qt::white);
        QPainter p(&padded);
        p.drawImage(0, 0, img);
        p.end();
        scaled = padded;
    }
    const int w = scaled.width();
    const int h = scaled.height();

    int tw = 0, th = 0;
    QVector<float> data = imageToNchw(scaled, &tw, &th);

    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> shape{1, 3, h, w};
        Ort::Value input = Ort::Value::CreateTensor<float>(mem, data.data(), size_t(data.size()),
                                                           shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        const char *inName = d->det->GetInputNameAllocated(0, allocator).get();
        const char *outName = d->det->GetOutputNameAllocated(0, allocator).get();
        auto results = d->det->Run(Ort::RunOptions{nullptr}, &inName, &input, 1, &outName, 1);

        const auto &t = results.at(0);
        const auto info = t.GetTensorTypeAndShapeInfo();
        const auto dims = info.GetShape();   // [1, 1, H', W']
        if (dims.size() != 4) {
            if (error) *error = QStringLiteral("det 输出维度异常");
            return out;
        }
        out.mapH = int(dims[2]);
        out.mapW = int(dims[3]);
        const size_t total = info.GetElementCount();
        out.data.resize(int(total));
        std::memcpy(out.data.data(), t.GetTensorData<float>(), total * sizeof(float));
    } catch (const Ort::Exception &e) {
        if (error) *error = QStringLiteral("det 推理失败：%1").arg(QString::fromUtf8(e.what()));
        out.data.clear();
        out.mapW = out.mapH = 0;
    }
    return out;
}

RecProb OcrOnnxEngine::runRec(const QImage &crop, QString *error)
{
    RecProb out;
    if (error) error->clear();
    if (!loaded()) {
        if (error) *error = QStringLiteral("OCR 引擎未加载");
        return out;
    }

    // rec 输入：高固定 48，宽按长宽比缩放后对齐到 8 的倍数。
    // 若 crop 为空或过窄，直接返回空（无文字）。
    if (crop.width() <= 0 || crop.height() <= 0) {
        if (error) *error = QStringLiteral("rec 输入为空");
        return out;
    }
    int targetW = int(qreal(crop.width()) * qreal(kRecHeight) / qreal(crop.height()));
    targetW = qMax(8, (targetW + kRecAlign - 1) / kRecAlign * kRecAlign);
    const QImage scaled = crop.scaled(targetW, kRecHeight, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);

    int tw = 0, th = 0;
    QVector<float> data = imageToNchw(scaled, &tw, &th);

    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> shape{1, 3, kRecHeight, targetW};
        Ort::Value input = Ort::Value::CreateTensor<float>(mem, data.data(), size_t(data.size()),
                                                           shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        const char *inName = d->rec->GetInputNameAllocated(0, allocator).get();
        const char *outName = d->rec->GetOutputNameAllocated(0, allocator).get();
        auto results = d->rec->Run(Ort::RunOptions{nullptr}, &inName, &input, 1, &outName, 1);

        const auto &t = results.at(0);
        const auto info = t.GetTensorTypeAndShapeInfo();
        const auto dims = info.GetShape();   // [1, T, 6625]
        if (dims.size() != 3) {
            if (error) *error = QStringLiteral("rec 输出维度异常");
            return out;
        }
        out.timesteps = int(dims[1]);
        const int rawVocab = int(dims[2]);
        // 输出通道 6625，但字典只有 6622 字符 + blank + 空格 = 6624 类，
        // 第 6625 类是导出对齐 padding，恒为极小概率，只取前 6624 类。
        out.vocabSize = rawVocab >= 6625 ? 6624 : rawVocab;

        const float *src = t.GetTensorData<float>();
        out.data.resize(size_t(out.timesteps) * size_t(out.vocabSize));
        // 行主序拷贝：每行只取前 vocabSize 类（跳过 padding 类）
        for (int tstep = 0; tstep < out.timesteps; ++tstep) {
            std::memcpy(out.data.data() + size_t(tstep) * size_t(out.vocabSize),
                        src + size_t(tstep) * size_t(rawVocab),
                        size_t(out.vocabSize) * sizeof(float));
        }
    } catch (const Ort::Exception &e) {
        if (error) *error = QStringLiteral("rec 推理失败：%1").arg(QString::fromUtf8(e.what()));
        out.data.clear();
        out.timesteps = out.vocabSize = 0;
    }
    return out;
}

QStringList OcrOnnxEngine::keys() const { return d ? d->keys : QStringList(); }

} // namespace ocrengine

#else // !XTOOLS_HAVE_ONNX —— 无 onnxruntime 时的桩实现

namespace ocrengine {

OcrOnnxEngine::OcrOnnxEngine() : d(nullptr) {}
OcrOnnxEngine::~OcrOnnxEngine() {}

bool OcrOnnxEngine::isAvailable() { return false; }

QString OcrOnnxEngine::modelDir()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("plugins/ocr/models"));
}

bool OcrOnnxEngine::load(QString *error)
{
    if (error) *error = QStringLiteral("本构建未启用 ONNX OCR（未配置 onnxruntime）");
    return false;
}

bool OcrOnnxEngine::loaded() const { return false; }

DetMap OcrOnnxEngine::runDet(const QImage &, QString *error)
{
    if (error) *error = QStringLiteral("本构建未启用 ONNX OCR");
    return {};
}

RecProb OcrOnnxEngine::runRec(const QImage &, QString *error)
{
    if (error) *error = QStringLiteral("本构建未启用 ONNX OCR");
    return {};
}

QStringList OcrOnnxEngine::keys() const { return {}; }

} // namespace ocrengine

#endif // XTOOLS_HAVE_ONNX

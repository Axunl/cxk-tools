#pragma once

// ---------------------------------------------------------------------------
// OCR 的 ONNX Runtime 推理封装。这是插件里唯一 include onnxruntime 头文件的地方
// （通过 .cpp 里 include，头文件只暴露 Qt 值类型）。
//
// 为什么头文件不暴露 Ort:: 类型：
//   * onnxruntime 的 C++ API 头文件（onnxruntime_cxx_api.h）较重，且只在
//     OcrOnnx.cpp 里需要；后处理（OcrPostprocess.h）与上层（OcrEngine.cpp）
//     都只依赖 Qt 值类型，若头文件暴露 Ort:: 会把 onnxruntime 拖进所有
//     include 者，破坏「纯逻辑 header-only 可脱离 onnxruntime 编译测试」的约定。
//   * 找不到 onnxruntime（未装/未配 -DONNXRUNTIME_ROOT）时，本文件仍能编译，
//     只是 OcrOnnxEngine::isAvailable() 恒返回 false，上层自动回退旧后端。
//
// 设计为「可选依赖」：XTOOLS_HAVE_ONNX 未定义时，本头文件退化为一个只有
// isAvailable() 恒 false 的桩，所有推理函数都返回错误，保证三端都能编译。
// ---------------------------------------------------------------------------

#include "OcrText.h"

#include <QImage>
#include <QString>
#include <QVector>

namespace ocrengine {

/// det 的一次前向结果：概率图（行主序，尺寸 mapW × mapH，值域 [0,1]）。
struct DetMap
{
    QVector<float> data;
    int mapW = 0;
    int mapH = 0;
    int origW = 0;   ///< det 输入在「缩放后、pad 前」的宽度，用于坐标回算
    int origH = 0;   ///< 同上，高度
    bool valid() const { return mapW > 0 && mapH > 0 && data.size() >= mapW * mapH; }
};

/// rec 的一次前向结果：CTC 概率（timesteps × vocabSize，已过 softmax）。
struct RecProb
{
    QVector<float> data;
    int timesteps = 0;
    int vocabSize = 0;
    bool valid() const
    {
        return timesteps > 0 && vocabSize > 0 && data.size() >= timesteps * vocabSize;
    }
};

/// ONNX Runtime 推理引擎（PP-OCRv4 det + rec）。
///
/// 用法：先 load()，成功后依次 runDet() 拿概率图、对每个 det 框 crop 后 runRec()
/// 拿识别概率。det 与 rec 共用一个对象，Session 只建一次（复用，避免重复加载模型）。
/// 所有函数都是同步阻塞（与 OcrEngine::recognize 的既定架构一致）。
class OcrOnnxEngine
{
public:
    OcrOnnxEngine();
    ~OcrOnnxEngine();

    // 禁止拷贝（持有 Ort::Session 等不可复制资源）
    OcrOnnxEngine(const OcrOnnxEngine &) = delete;
    OcrOnnxEngine &operator=(const OcrOnnxEngine &) = delete;

    /// onnxruntime + 模型 + keys 是否就绪。未定义 XTOOLS_HAVE_ONNX 时恒 false。
    static bool isAvailable();

    /// 模型目录：<可执行文件同级>/plugins/ocr/models（用 QDir 拼，不写死分隔符）。
    static QString modelDir();

    /// 加载 det/rec 两个 Session 与 keys 字符表。失败返回 false 并填 error。
    /// 幂等：重复调用会先释放旧 Session 再重建。
    bool load(QString *error);

    /// 是否已成功加载。
    bool loaded() const;

    /// 整图 → det 概率图。img 应已按「长边 ≤ 960」缩放过（调用方负责），
    /// 本函数内部会 pad 到 32 的倍数再前向。返回的 DetMap 里 origW/origH 是
    /// pad 前的尺寸，用于把框坐标映射回传入 img 的坐标系。
    DetMap runDet(const QImage &img, QString *error);

    /// 文字框 crop 图 → rec 概率。crop 会缩放到 rec 固定输入（H=48，宽按比例）。
    RecProb runRec(const QImage &crop, QString *error);

    /// 识别字符表（ppocr_keys_v1.txt 的每一行）。load() 成功后非空。
    QStringList keys() const;

private:
    struct Impl;
    Impl *d = nullptr;   ///< PIMPL：把 Ort::Session 藏进 .cpp，头文件零 onnxruntime 依赖
};

} // namespace ocrengine

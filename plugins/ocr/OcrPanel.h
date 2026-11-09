#pragma once

// ---------------------------------------------------------------------------
// OCR 工具面板（插件内部使用，宿主只认 ITool）。
//
// 交互链路：截图框选 / 剪贴板图片 / 本地图片 → 落临时 PNG → OcrEngine 识别
// → OcrText 排版 → 结果文本框（可编辑）→ 复制或保存 TXT。
//
// 刻意不做的事，以及原因：
//   * 不注册第二个全局热键：项目既有的全局热键语义是「唤出首页搜索」，
//     属于已定型的交互约定（见 AGENTS.md 第二部分），改它要先跟使用者确认；
//     截图识别从面板按钮发起。
//   * 不做翻译 / 公式识别：前者必须联网或带本地模型，后者需要专门引擎，
//     都与「完全离线、零额外依赖」冲突；「表格文本」只是按空隙分列的 TSV，
//     界面文案如实说明它不是结构化表格识别。
//   * 面板不缓存任何状态到静态变量：宿主可能同时存在内嵌面板与独立窗口面板，
//     两份识别结果必须互不干扰。
// ---------------------------------------------------------------------------

#include "OcrText.h"

#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QVector>
#include <QWidget>

class OcrCaptureOverlay;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class OcrPanel : public QWidget
{
    Q_OBJECT

public:
    explicit OcrPanel(QWidget *parent = nullptr);
    ~OcrPanel() override;

    /// 宿主把首页搜索框里粘贴的图片带进来时调用：立刻识别，不等用户再点按钮。
    /// 同步执行（与「剪贴板图片」按钮同一条路径），空图片直接忽略。
    void setInputImage(const QImage &image);

private:
    /// 隐藏宿主窗口后延时截图；延时是为了让桌面完成重绘，否则会截到主窗口残影。
    void startCapture();
    void captureScreenNow();

    /// 把宿主窗口恢复到截图前的可见状态；截图成功/取消/面板销毁都要调一次。
    void restoreHostWindow();

    /// 同步识别一张图片并刷新结果区；不成功时保留旧结果，只把原因写进状态栏。
    void recognizePixmap(const QPixmap &image);

    void pasteFromClipboard();
    void openImageFile();

    /// 按当前下拉选项重新排版 m_words 并写回结果框（会覆盖手动修改，属预期行为）。
    void applyMode();

    void copyResult();
    void saveResult();
    void clearAll();

    void setStatus(const QString &text, bool ok);
    void showPreview(const QPixmap &image);
    void showIdlePreview();

    /// 隐藏主窗口到 grabWindow 之间的等待时间（毫秒）。
    static constexpr int kCaptureDelayMs = 260;

    QPushButton *m_captureBtn = nullptr;
    QPushButton *m_pasteBtn = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QComboBox *m_mode = nullptr;
    QPlainTextEdit *m_result = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_status = nullptr;

    QVector<ocrtext::Word> m_words;             ///< 最近一次识别的词，切换输出形态时复用
    QPointer<OcrCaptureOverlay> m_overlay;      ///< 浮层不设 parent，由面板兜底销毁
    QPointer<QWidget> m_hostWindow;             ///< 截图前隐藏的宿主顶层窗口
    bool m_hostWasVisible = false;
    bool m_capturePending = false;
    bool m_busy = false;                        ///< 同步识别期间防重入
};

#pragma once

// ---------------------------------------------------------------------------
// 二维码工具面板（插件内部使用，宿主只认 ITool）。
//
// 界面分两页，顶上用一对可切换的胶囊按钮选：
//   1. 生成：输入文本 → 实时出图（防抖），可保存 PNG / 复制图片；
//   2. 识别：拖入 / 选择 / 粘贴图片 → 显示内容与版本、容错等级等信息。
//
// 几个刻意的取舍：
//   * 生成不设「生成」按钮 —— 编码是毫秒级的纯计算，输入即出图更顺手；
//     防抖只是为了不在每个按键上都重建一遍矩阵与图片。
//   * 识别结果只读：解码内容来自外部图片，允许就地编辑会让人分不清
//     "识别出来的"和"我改过的"，要改内容请到生成页。
//   * 面板不存任何静态状态：宿主可能同时存在内嵌面板与独立窗口两份
//     （见 docs/PLUGIN.md 第 4 节），静态变量会让两边串内容。
//   * 图片不落盘：识别直接用 QImage 在内存里做，临时文件既不产生也不用清理。
// ---------------------------------------------------------------------------

#include <QImage>
#include <QString>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTimer;
class QComboBox;

class QrCodePanel : public QWidget
{
    Q_OBJECT

public:
    explicit QrCodePanel(QWidget *parent = nullptr);

protected:
    /// 支持把图片直接拖进面板（拖到哪一页都会切到识别页并立刻解码）。
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    /// 给"图片投放区"补点击行为（QLabel 本身没有 clicked 信号）。
    bool eventFilter(QObject *watched, QEvent *event) override;

    /// 投放区高度随面板变化，缩略图要在窗口尺寸变化后重新贴合。
    void resizeEvent(QResizeEvent *event) override;

private:
    /// 状态行三种语义：提示（灰）、成功（绿）、错误（红）。
    enum class StatusKind { Hint, Ok, Error };

    // ---------------------------------------------------------------- 页面搭建

    QWidget *buildCreatePage();
    QWidget *buildDecodePage();

    /// 顶部胶囊按钮：两个互斥的 QPushButton（autoExclusive）比 QTabWidget 好配色，
    /// 也不会带进 QTabWidget 自带的面板边框。
    QWidget *buildPageSwitcher();

    // ---------------------------------------------------------------- 生成

    /// 输入变化时重置防抖定时器。
    void scheduleEncode();

    /// 真正编码并按当前尺寸渲染，更新预览与状态行。
    void encodeNow();

    void savePng();
    void copyImage();
    void updatePreviewPixmap();

    // ---------------------------------------------------------------- 识别

    void chooseImageFile();
    void pasteImage();
    void clearDecoded();

    /// 统一的识别入口：source 只是给状态行看的来源说明（"剪贴板" / 文件名）。
    void loadImage(const QImage &image, const QString &source);

    /// 按投放区当前尺寸重画缩略图（载入图片时、窗口尺寸变化时都要调）。
    void updateThumbnail();

    void copyDecodedText();

    // ---------------------------------------------------------------- 小工具

    void switchPage(int index);
    void setCreateStatus(const QString &text, StatusKind kind);
    void setDecodeStatus(const QString &text, StatusKind kind);

    // ---------------------------------------------------------------- 控件

    QStackedWidget *m_pages = nullptr;
    QPushButton    *m_createTab = nullptr;
    QPushButton    *m_decodeTab = nullptr;

    // 生成页
    QPlainTextEdit *m_content = nullptr;
    QComboBox      *m_eccBox = nullptr;
    QComboBox      *m_scaleBox = nullptr;
    QLabel         *m_preview = nullptr;
    QLabel         *m_createStatus = nullptr;
    QPushButton    *m_saveButton = nullptr;
    QPushButton    *m_copyImageButton = nullptr;
    QTimer         *m_debounce = nullptr;

    // 识别页
    QWidget        *m_decodePage = nullptr;
    QLabel         *m_dropZone = nullptr;
    QPlainTextEdit *m_result = nullptr;
    QLabel         *m_decodeStatus = nullptr;
    QPushButton    *m_copyTextButton = nullptr;
    QPushButton    *m_clearButton = nullptr;

    // 状态
    QImage  m_renderedImage;   ///< 最近一次生成的二维码（保存/复制用）
    QImage  m_sourceImage;     ///< 最近一次载入待识别的图片（缩略图用）
    QString m_decodedText;     ///< 最近一次识别结果（复制用）
};

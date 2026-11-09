#include "OcrPanel.h"

#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用）

#include "OcrCapture.h"
#include "OcrEngine.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

OcrPanel::OcrPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐（与 CalcPanel / IpQueryPanel 的约定一致）
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(10);

    // ---------------------------------------------------------------- 工具条
    auto *bar = new QHBoxLayout;
    bar->setSpacing(8);

    m_captureBtn = new QPushButton(QStringLiteral("截图识别"), this);
    m_captureBtn->setProperty("accent", true);   // 全局 qss 的强调按钮样式
    m_captureBtn->setToolTip(QStringLiteral("隐藏窗口后拖拽框选屏幕区域"));
    bar->addWidget(m_captureBtn);

    m_pasteBtn = new QPushButton(QStringLiteral("剪贴板图片"), this);
    bar->addWidget(m_pasteBtn);

    m_openBtn = new QPushButton(QStringLiteral("打开图片…"), this);
    bar->addWidget(m_openBtn);

    bar->addSpacing(12);
    auto *modeLabel = new QLabel(QStringLiteral("输出："), this);
    bar->addWidget(modeLabel);
    m_mode = new QComboBox(this);
    m_mode->addItem(QStringLiteral("普通文字"));
    m_mode->addItem(QStringLiteral("表格文本（Tab 分列）"));
    m_mode->setToolTip(QStringLiteral("表格文本按词间空隙分列，可直接粘贴到表格软件；"
                                      "不是结构化表格识别"));
    bar->addWidget(m_mode);

    bar->addStretch(1);
    m_copyBtn = new QPushButton(QStringLiteral("复制结果"), this);
    m_saveBtn = new QPushButton(QStringLiteral("保存 TXT"), this);
    m_clearBtn = new QPushButton(QStringLiteral("清空"), this);
    bar->addWidget(m_copyBtn);
    bar->addWidget(m_saveBtn);
    bar->addWidget(m_clearBtn);
    layout->addLayout(bar);

    // ---------------------------------------------------------------- 内容区
    auto *body = new QHBoxLayout;
    body->setSpacing(12);

    m_result = new QPlainTextEdit(this);
    m_result->setPlaceholderText(
        QStringLiteral("点「截图识别」后拖拽框选，或用「剪贴板图片 / 打开图片」导入一张图片。\n"
                       "识别出的文字会出现在这里，可以直接编辑、复制或保存。"));
    // 插件内控件用局部样式：全局 qss 只按 objectName 命中主程序控件。
    // 颜色写 @令牌，换肤时宿主会按模板重算（见 api/ThemePalette.h）。
    xtools::theme::applyStyle(m_result, QStringLiteral(
        "QPlainTextEdit{background:@bg;border:1px solid @border;border-radius:8px;"
        "padding:12px;font-size:14px;color:@text;}"));
    body->addWidget(m_result, 1);

    m_preview = new QLabel(this);
    m_preview->setFixedWidth(220);
    m_preview->setMinimumHeight(160);
    m_preview->setAlignment(Qt::AlignCenter);
    xtools::theme::applyStyle(m_preview, QStringLiteral(
        "QLabel{background:@bg;border:1px solid @border;border-radius:8px;"
        "color:@text-faint;font-size:12px;}"));
    body->addWidget(m_preview);
    layout->addLayout(body, 1);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    showIdlePreview();
    setStatus(QStringLiteral("引擎：%1。截图识别从上方按钮发起，按 Esc 可取消框选。")
                  .arg(ocrengine::backendName()), true);

    connect(m_captureBtn, &QPushButton::clicked, this, &OcrPanel::startCapture);
    connect(m_pasteBtn, &QPushButton::clicked, this, &OcrPanel::pasteFromClipboard);
    connect(m_openBtn, &QPushButton::clicked, this, &OcrPanel::openImageFile);
    connect(m_copyBtn, &QPushButton::clicked, this, &OcrPanel::copyResult);
    connect(m_saveBtn, &QPushButton::clicked, this, &OcrPanel::saveResult);
    connect(m_clearBtn, &QPushButton::clicked, this, &OcrPanel::clearAll);
    // 切换输出形态会重排并覆盖手工编辑，这是预期行为（结果框只是本次识别的视图）
    connect(m_mode, &QComboBox::currentIndexChanged, this, &OcrPanel::applyMode);
}

OcrPanel::~OcrPanel()
{
    // 浮层不设 parent（见 OcrCapture.h 的说明），面板销毁时必须自己收掉，
    // 否则截屏遮罩会留在桌面上；同时把宿主窗口恢复成可见，避免界面「消失」。
    if (m_overlay) {
        m_overlay->deleteLater();
        m_overlay = nullptr;
    }
    if (m_capturePending)
        restoreHostWindow();
}

void OcrPanel::startCapture()
{
    if (m_busy || m_capturePending)
        return;

    m_hostWindow = window();
    m_hostWasVisible = m_hostWindow && m_hostWindow->isVisible();
    m_capturePending = true;
    if (m_hostWindow)
        m_hostWindow->hide();   // 不 hide 的话浮层会盖住主窗口，用户截不到后面的内容

    setStatus(QStringLiteral("请在屏幕上拖拽框选要识别的区域，Esc 取消…"), true);

    // 延迟抓屏：hide() 之后桌面需要重绘一轮，立刻 grabWindow 会截到主窗口残影。
    // 以 this 为 context，面板若在等待期间被销毁，这个回调自动作废。
    QTimer::singleShot(kCaptureDelayMs, this, [this] { captureScreenNow(); });
}

void OcrPanel::captureScreenNow()
{
    if (!m_capturePending)
        return;

    // 只覆盖光标所在的屏幕：跨屏拖拽需要多块 overlay 协同，代价与收益不成比例，
    // 这里给出的是单屏框选（多屏用户把要识别的窗口移到光标所在屏即可）。
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen) {
        restoreHostWindow();
        setStatus(QStringLiteral("找不到可用的屏幕，无法截图"), false);
        return;
    }

    const QRect geometry = screen->geometry();
    QPixmap shot = screen->grabWindow(0);
    if (shot.isNull()) {
        restoreHostWindow();
        // Wayland 等会话下 Qt 拿不到屏幕像素，必须给可读原因而不是静默失败
        setStatus(QStringLiteral("屏幕截图失败：当前会话可能不允许抓取屏幕"
                                 "（Wayland 下常见），请改用「打开图片」或「剪贴板图片」"), false);
        return;
    }

    // 不猜 platform 的 DPR 行为，直接用「截图像素宽 / 屏幕逻辑宽」反推缩放比：
    // 有的平台 grabWindow 返回设备像素图，有的返回逻辑像素图，写死 DPR 会截歪。
    const qreal dpr = geometry.width() > 0
        ? qMax(1.0, qreal(shot.width()) / qreal(geometry.width())) : 1.0;
    shot.setDevicePixelRatio(dpr);

    auto *overlay = new OcrCaptureOverlay(
        geometry, shot, dpr,
        [this](const QPixmap &cropped) {
            restoreHostWindow();
            recognizePixmap(cropped);
        },
        [this] {
            restoreHostWindow();
            setStatus(QStringLiteral("已取消截图"), true);
        });
    m_overlay = overlay;
    overlay->show();
    overlay->raise();
    overlay->activateWindow();
    overlay->setFocus();   // Esc 取消依赖浮层拿到键盘焦点
}

void OcrPanel::restoreHostWindow()
{
    m_capturePending = false;
    if (m_hostWindow && m_hostWasVisible) {
        m_hostWindow->show();
        m_hostWindow->raise();
        m_hostWindow->activateWindow();
    }
    m_hostWindow = nullptr;
}

void OcrPanel::recognizePixmap(const QPixmap &image)
{
    if (m_busy || image.isNull())
        return;

    m_busy = true;
    setStatus(QStringLiteral("正在识别…"), true);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    // 只同步重绘状态栏。不能用 processEvents()：那会进入事件循环，用户可能在
    // 「识别中」切页，宿主随即销毁面板，而此刻函数栈仍持有 this（悬空指针）。
    m_status->repaint();

    // 后端接收的是文件路径（PowerShell / tesseract 都是命令行进程）。
    // 用 QTemporaryDir + 直接保存，而不是 QTemporaryFile：后者会一直持有文件句柄，
    // 外部进程打开图片时可能报「正由另一进程使用」。
    // 用 QImage（而非 QPixmap）保存：QImage 的 PNG 编码更可控，且能先规范成
    // 8-bit RGB/RGBA，避免 QPixmap 在部分平台上保存出 16-bit 或带异常元数据的
    // PNG，导致 WinRT BitmapDecoder 解码失败（表现为「一个或多个错误」）。
    // 临时目录在本函数结束时析构，连 PNG 一起删掉，不在磁盘留垃圾。
    QTemporaryDir imageDir;
    ocrengine::Result result;
    if (!imageDir.isValid()) {
        result.error = QStringLiteral("无法创建临时目录（系统临时目录不可写）");
    } else {
        const QString imagePath = QDir(imageDir.path()).filePath(QStringLiteral("cxk-ocr.png"));
        QImage flat = image.toImage();
        if (flat.hasAlphaChannel())
            flat = flat.convertToFormat(QImage::Format_RGBA8888);
        else
            flat = flat.convertToFormat(QImage::Format_RGB888);
        if (flat.save(imagePath, "PNG"))
            result = ocrengine::recognize(imagePath);
        else
            result.error = QStringLiteral("图片保存失败，无法交给 OCR 引擎");
    }

    QApplication::restoreOverrideCursor();
    m_busy = false;

    if (!result.error.isEmpty()) {
        // 失败保留上一次的结果，避免一次引擎故障把用户已改好的文字冲掉
        setStatus(result.error, false);
        return;
    }
    if (result.words.isEmpty()) {
        setStatus(QStringLiteral("未识别到文字：换一张更清晰的图，或把框选区域放大一点再试"), false);
        return;
    }

    m_words = result.words;
    showPreview(image);
    applyMode();
    setStatus(QStringLiteral("识别完成 · %1 个字符 · 引擎：%2")
                  .arg(ocrtext::countNonSpace(m_result->toPlainText()))
                  .arg(result.backend), true);
}

void OcrPanel::setInputImage(const QImage &image)
{
    // 与「剪贴板图片」按钮完全相同的一条路径：落临时 PNG → 同步识别 → 排版
    if (image.isNull())
        return;
    recognizePixmap(QPixmap::fromImage(image));
}

void OcrPanel::pasteFromClipboard()
{
    QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull()) {
        // 某些程序只往剪贴板放 pixmap（截图工具常见），image() 拿不到
        const QPixmap pixmap = QGuiApplication::clipboard()->pixmap();
        if (!pixmap.isNull())
            image = pixmap.toImage();
    }
    if (image.isNull()) {
        setStatus(QStringLiteral("剪贴板里没有图片：先复制一张图，或改用「打开图片」"), false);
        return;
    }
    recognizePixmap(QPixmap::fromImage(image));
}

void OcrPanel::openImageFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要识别的图片"), QString(),
        QStringLiteral("图片文件 (*.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    const QPixmap image(path);
    if (image.isNull()) {
        setStatus(QStringLiteral("无法读取这张图片（格式不支持或文件已损坏）"), false);
        return;
    }
    recognizePixmap(image);
}

void OcrPanel::applyMode()
{
    if (m_words.isEmpty())
        return;
    const ocrtext::OutputMode mode = m_mode->currentIndex() == 1
        ? ocrtext::OutputMode::Table : ocrtext::OutputMode::Plain;
    m_result->setPlainText(ocrtext::render(m_words, mode));
}

void OcrPanel::copyResult()
{
    const QString text = m_result->toPlainText();
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral("还没有可复制的识别结果"), false);
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    setStatus(QStringLiteral("已复制 %1 个字符到剪贴板")
                  .arg(ocrtext::countNonSpace(text)), true);
}

void OcrPanel::saveResult()
{
    const QString text = m_result->toPlainText();
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral("还没有可保存的识别结果"), false);
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存识别结果"), QString(),
        QStringLiteral("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(QStringLiteral("保存失败：无法写入该文件（权限或路径问题）"), false);
        return;
    }
    // UTF-8 无 BOM，与仓库内文本文件的编码约定一致
    (void)file.write(text.toUtf8());
    file.close();
    setStatus(QStringLiteral("已保存到 %1").arg(path), true);
}

void OcrPanel::clearAll()
{
    m_words.clear();
    m_result->clear();
    showIdlePreview();
    setStatus(QStringLiteral("引擎：%1。截图识别从上方按钮发起，按 Esc 可取消框选。")
                  .arg(ocrengine::backendName()), true);
}

void OcrPanel::setStatus(const QString &text, bool ok)
{
    m_status->setText(text);
    xtools::theme::applyStyle(m_status, QStringLiteral("font-size:12px;color:@%1;")
                                             .arg(xtools::theme::tokenName(
                                                 ok ? xtools::theme::Color::TextFaint
                                                    : xtools::theme::Color::Danger)));
}

void OcrPanel::showPreview(const QPixmap &image)
{
    // 预览按固定框缩放即可，识别用的是原始分辨率的临时 PNG，不在这里损失清晰度
    m_preview->setPixmap(image.scaled(QSize(200, 320), Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
}

void OcrPanel::showIdlePreview()
{
    m_preview->setPixmap(QPixmap());
    m_preview->setText(QStringLiteral("图片预览"));
}

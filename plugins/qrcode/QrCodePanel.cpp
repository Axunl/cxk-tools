#include "QrCodePanel.h"

#include "ThemePalette.h"
#include "LayoutMetrics.h"   // 颜色令牌（宿主与插件共用）

#include "QrDecode.h"
#include "QrEncode.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

/// 输入防抖：编码本身是毫秒级，防抖只为避免每敲一个键都重建矩阵 + 重绘预览。
constexpr int kDebounceMs = 160;

/// 预览区边长。固定尺寸让左右两栏在窗口拉伸时保持稳定，二维码不会被拉变形。
constexpr int kPreviewSide = 320;

/// 状态行配色，与 regex / textkit 等插件同一套语义令牌（浅/深各一套色值）。
namespace palette {
constexpr auto hint = xtools::theme::Color::TextMuted;
constexpr auto ok = xtools::theme::Color::SuccessText;
constexpr auto error = xtools::theme::Color::Danger;
} // namespace palette

/// 状态行统一出口：模板里带 @令牌，换肤时宿主会重算。
void applyStatus(QLabel *label, const QString &text, xtools::theme::Color token)
{
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@%1;")
                                         .arg(xtools::theme::tokenName(token)));
    label->setText(text);
}

/// 小节标题：12px、灰蓝、加粗。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    return label;
}

/// 辅助说明：12px 灰色小字。
QLabel *makeHintLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    applyStatus(label, text, palette::hint);
    return label;
}

/// 把 QImage 缩到能放进指定方框的尺寸（保持比例、平滑缩放）。
/// 只用于界面预览；保存与复制走的是原始图像，不经过这里。
QPixmap fitPixmap(const QImage &image, const QSize &box)
{
    if (image.isNull())
        return QPixmap();
    return QPixmap::fromImage(image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

/// 从 MIME 数据里取一张图：优先 image 数据，其次第一个本地图片文件。
/// 返回值可能为空，调用方据此提示"拖进来的不是图片"。
QImage imageFromMime(const QMimeData *mime)
{
    if (!mime)
        return QImage();

    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull())
            return image;
    }

    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString path = url.toLocalFile();
            QImageReader reader(path);
            reader.setAutoTransform(true);   // 手机照片按 EXIF 方向摆正
            const QImage image = reader.read();
            if (!image.isNull())
                return image;
        }
    }
    return QImage();
}

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

QrCodePanel::QrCodePanel(QWidget *parent)
    : QWidget(parent)
{
    // 整块面板都接受拖放：拖到生成页也直接切到识别页，省掉"先点一下页签"
    setAcceptDrops(true);

    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐；上方留白由宿主补
    layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    layout->setSpacing(12);

    layout->addWidget(buildPageSwitcher());

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildCreatePage());
    m_pages->addWidget(buildDecodePage());
    layout->addWidget(m_pages, 1);

    // ---- 防抖与联动 ----
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &QrCodePanel::encodeNow);
    connect(m_content, &QPlainTextEdit::textChanged, this, &QrCodePanel::scheduleEncode);
    connect(m_eccBox, &QComboBox::currentIndexChanged, this, &QrCodePanel::scheduleEncode);
    connect(m_scaleBox, &QComboBox::currentIndexChanged, this, &QrCodePanel::scheduleEncode);

    // 识别页的 Ctrl+V：作用域限定在识别页，生成页里的编辑框仍然走自己的粘贴。
    auto *pasteShortcut = new QShortcut(QKeySequence::Paste, m_decodePage);
    pasteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(pasteShortcut, &QShortcut::activated, this, &QrCodePanel::pasteImage);

    encodeNow();   // 初始空状态：清预览、写提示
}

// ---------------------------------------------------------------------------
// 页面搭建
// ---------------------------------------------------------------------------

QWidget *QrCodePanel::buildPageSwitcher()
{
    auto *host = new QWidget(this);
    auto *row = new QHBoxLayout(host);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    m_createTab = new QPushButton(QStringLiteral("生成"), host);
    m_decodeTab = new QPushButton(QStringLiteral("识别"), host);
    for (QPushButton *button : {m_createTab, m_decodeTab}) {
        button->setObjectName(QStringLiteral("qrTabBtn"));
        button->setCheckable(true);
        button->setAutoExclusive(true);   // 同一个父对象下自动互斥，不需要 QButtonGroup
        button->setCursor(Qt::PointingHandCursor);
        row->addWidget(button);
    }
    m_createTab->setChecked(true);

    row->addStretch(1);
    row->addWidget(makeHintLabel(QStringLiteral("全部离线处理，图片不会离开本机"), host));

    // 胶囊按钮的就地样式：全局 qss 的 QPushButton 带边框与内边距，这里全部覆盖。
    xtools::theme::applyStyle(host, QStringLiteral(
        "QPushButton#qrTabBtn{background:transparent;border:none;border-radius:8px;"
        "padding:6px 18px;color:@text-muted;font-weight:600;}"
        "QPushButton#qrTabBtn:hover{background:@bg-subtle;}"
        "QPushButton#qrTabBtn:checked{background:@accent-soft;color:@accent-text;}"));

    connect(m_createTab, &QPushButton::clicked, this, [this] { switchPage(0); });
    connect(m_decodeTab, &QPushButton::clicked, this, [this] { switchPage(1); });
    return host;
}

QWidget *QrCodePanel::buildCreatePage()
{
    auto *page = new QWidget(this);
    auto *row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(18);

    // ---- 左栏：内容 + 参数 + 状态 ----
    auto *left = new QVBoxLayout;
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(8);

    left->addWidget(makeCaption(QStringLiteral("内容"), page));

    m_content = new QPlainTextEdit(page);
    m_content->setPlaceholderText(
        QStringLiteral("输入要生成二维码的内容：文本、网址、Wi-Fi 配置、中文都可以…"));
    m_content->setMinimumHeight(140);
    m_content->setToolTip(QStringLiteral("内容按 UTF-8 编码；一个汉字占 3 字节，会更快吃满容量"));
    left->addWidget(m_content, 1);

    auto *options = new QHBoxLayout;
    options->setSpacing(8);
    options->addWidget(makeHintLabel(QStringLiteral("容错等级"), page));

    m_eccBox = new QComboBox(page);
    m_eccBox->addItem(QStringLiteral("L · 约 7%"), int(qrcode::Ecc::Low));
    m_eccBox->addItem(QStringLiteral("M · 约 15%"), int(qrcode::Ecc::Medium));
    m_eccBox->addItem(QStringLiteral("Q · 约 25%"), int(qrcode::Ecc::Quartile));
    m_eccBox->addItem(QStringLiteral("H · 约 30%"), int(qrcode::Ecc::High));
    m_eccBox->setCurrentIndex(1);
    m_eccBox->setToolTip(QStringLiteral(
        "能恢复的损坏比例。等级越高越抗遮挡/磨损，但同样内容需要更大的码；日常用 M 即可"));
    options->addWidget(m_eccBox);

    options->addSpacing(12);
    options->addWidget(makeHintLabel(QStringLiteral("模块尺寸"), page));

    m_scaleBox = new QComboBox(page);
    m_scaleBox->addItem(QStringLiteral("自动"), 0);
    for (const int scale : {4, 6, 8, 12})
        m_scaleBox->addItem(QStringLiteral("%1 px").arg(scale), scale);
    m_scaleBox->setToolTip(QStringLiteral("每个模块占多少像素。自动会按预览尺寸与版本挑选，保存时按这个分辨率出图"));
    options->addWidget(m_scaleBox);

    options->addStretch(1);
    left->addLayout(options);

    m_createStatus = new QLabel(page);
    m_createStatus->setWordWrap(true);
    m_createStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left->addWidget(m_createStatus);

    row->addLayout(left, 1);

    // ---- 右栏：预览 + 动作 ----
    auto *right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(10);

    m_preview = new QLabel(page);
    m_preview->setObjectName(QStringLiteral("qrPreview"));
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFixedSize(kPreviewSide, kPreviewSide);
    // 二维码必须放在浅底上：深色主题下若跟着变深，扫描器可能读不出来。
    // 所以这里的 #FFFFFF 是**故意的字面量**，不换成 @bg（那会跟着主题变深）。
    xtools::theme::applyStyle(m_preview, QStringLiteral(
        "QLabel#qrPreview{background:#FFFFFF;border:1px solid @border-subtle;border-radius:12px;}"));
    right->addWidget(m_preview, 0, Qt::AlignHCenter);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_saveButton = new QPushButton(QStringLiteral("保存 PNG"), page);
    m_saveButton->setToolTip(QStringLiteral("按当前模块尺寸导出 PNG（带 4 模块静区）"));
    m_copyImageButton = new QPushButton(QStringLiteral("复制图片"), page);
    m_copyImageButton->setProperty("accent", true);   // 全局 qss 的强调色按钮
    m_copyImageButton->setToolTip(QStringLiteral("把二维码图片放进剪贴板，可直接贴到聊天窗口或文档里"));
    actions->addWidget(m_saveButton);
    actions->addWidget(m_copyImageButton);
    right->addLayout(actions);

    connect(m_saveButton, &QPushButton::clicked, this, &QrCodePanel::savePng);
    connect(m_copyImageButton, &QPushButton::clicked, this, &QrCodePanel::copyImage);

    row->addLayout(right, 0);
    return page;
}

QWidget *QrCodePanel::buildDecodePage()
{
    m_decodePage = new QWidget(this);
    auto *row = new QHBoxLayout(m_decodePage);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(18);

    // ---- 左栏：投放区 + 按钮 ----
    auto *left = new QVBoxLayout;
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(8);
    left->addWidget(makeCaption(QStringLiteral("图片"), m_decodePage));

    m_dropZone = new QLabel(m_decodePage);
    m_dropZone->setObjectName(QStringLiteral("qrDropZone"));
    m_dropZone->setAlignment(Qt::AlignCenter);
    m_dropZone->setWordWrap(true);
    // 宽度定死（左栏宽度由它撑出来），高度可以随面板长一点，但封在 4:3 以内：
    // 面板往往很高，任由它拉满会变成一个又细又长的空框，比留白更难看。
    m_dropZone->setMinimumSize(240, 180);
    m_dropZone->setMaximumSize(240, 320);
    m_dropZone->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_dropZone->setCursor(Qt::PointingHandCursor);
    m_dropZone->setText(QStringLiteral("点击选择图片\n或把图片拖进来\n（Ctrl+V 粘贴剪贴板图片）"));
    xtools::theme::applyStyle(m_dropZone, QStringLiteral(
        "QLabel#qrDropZone{background:@bg-alt;border:1px dashed @scrollbar;border-radius:12px;"
        "color:@text-muted;font-size:12px;}"));
    m_dropZone->setToolTip(QStringLiteral("支持 PNG / JPG / BMP / GIF / WebP"));
    m_dropZone->installEventFilter(this);
    left->addWidget(m_dropZone);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    auto *chooseButton = new QPushButton(QStringLiteral("选择图片…"), m_decodePage);
    auto *pasteButton = new QPushButton(QStringLiteral("粘贴图片"), m_decodePage);
    m_copyTextButton = new QPushButton(QStringLiteral("复制内容"), m_decodePage);
    m_copyTextButton->setProperty("accent", true);
    m_clearButton = new QPushButton(QStringLiteral("清空"), m_decodePage);
    buttons->addWidget(chooseButton);
    buttons->addWidget(pasteButton);
    left->addLayout(buttons);

    auto *buttons2 = new QHBoxLayout;
    buttons2->setSpacing(8);
    buttons2->addWidget(m_copyTextButton);
    buttons2->addWidget(m_clearButton);
    buttons2->addStretch(1);
    left->addLayout(buttons2);

    // 投放区有高度上限，剩下来的纵向余量留在这里，按钮就不会被顶到面板最底下
    left->addStretch(1);
    row->addLayout(left, 0);

    // ---- 右栏：结果 ----
    auto *right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(8);
    right->addWidget(makeCaption(QStringLiteral("识别结果"), m_decodePage));

    m_result = new QPlainTextEdit(m_decodePage);
    m_result->setReadOnly(true);
    m_result->setPlaceholderText(QStringLiteral("识别成功后，二维码里的内容会出现在这里"));
    right->addWidget(m_result, 1);

    m_decodeStatus = new QLabel(m_decodePage);
    m_decodeStatus->setWordWrap(true);
    m_decodeStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    right->addWidget(m_decodeStatus);

    row->addLayout(right, 1);

    connect(chooseButton, &QPushButton::clicked, this, &QrCodePanel::chooseImageFile);
    connect(pasteButton, &QPushButton::clicked, this, &QrCodePanel::pasteImage);
    connect(m_copyTextButton, &QPushButton::clicked, this, &QrCodePanel::copyDecodedText);
    connect(m_clearButton, &QPushButton::clicked, this, &QrCodePanel::clearDecoded);

    return m_decodePage;
}

// ---------------------------------------------------------------------------
// 生成
// ---------------------------------------------------------------------------

void QrCodePanel::scheduleEncode()
{
    m_debounce->start();
}

void QrCodePanel::encodeNow()
{
    const QString text = m_content->toPlainText();
    if (text.isEmpty()) {
        m_renderedImage = QImage();
        m_saveButton->setEnabled(false);
        m_copyImageButton->setEnabled(false);
        updatePreviewPixmap();
        setCreateStatus(QStringLiteral("输入内容即自动生成，不需要点按钮"), StatusKind::Hint);
        return;
    }

    qrcode::EncodeOptions options;
    options.ecc = static_cast<qrcode::Ecc>(m_eccBox->currentData().toInt());
    options.scale = m_scaleBox->currentData().toInt();
    options.targetPixels = kPreviewSide - 20;

    const qrcode::Code code = qrcode::encode(text, options);
    if (!code.ok) {
        m_renderedImage = QImage();
        m_saveButton->setEnabled(false);
        m_copyImageButton->setEnabled(false);
        updatePreviewPixmap();
        setCreateStatus(code.error, StatusKind::Error);
        return;
    }

    m_renderedImage = qrcode::render(code, options);
    updatePreviewPixmap();
    m_saveButton->setEnabled(!m_renderedImage.isNull());
    m_copyImageButton->setEnabled(!m_renderedImage.isNull());

    const QString status =
        QStringLiteral("版本 v%1 · %2×%2 模块 · 容错 %3 · 占用 %4 / %5 字节 · 每模块 %6 像素")
            .arg(code.version)
            .arg(code.size)
            .arg(qrcode::eccName(options.ecc))
            .arg(code.dataBytes)
            .arg(code.capacityBytes)
            .arg(qrcode::autoScale(code, options));
    setCreateStatus(status, StatusKind::Ok);
}

void QrCodePanel::updatePreviewPixmap()
{
    if (m_renderedImage.isNull()) {
        m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("还没有内容"));
        // 预览底始终为白（见上），占位文字也只能是深色，故仍是字面量
        xtools::theme::applyStyle(m_preview, QStringLiteral(
            "QLabel#qrPreview{background:#FFFFFF;border:1px solid @border-subtle;border-radius:12px;"
            "color:#94A3B8;font-size:12px;}"));
        return;
    }

    m_preview->setText(QString());
    xtools::theme::applyStyle(m_preview, QStringLiteral(
        "QLabel#qrPreview{background:#FFFFFF;border:1px solid @border-subtle;border-radius:12px;}"));
    // 预览留 12px 内边距，免得二维码贴到圆角边框上
    m_preview->setPixmap(fitPixmap(m_renderedImage,
                                   QSize(kPreviewSide - 24, kPreviewSide - 24)));
}

void QrCodePanel::savePng()
{
    if (m_renderedImage.isNull())
        return;

    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (baseDir.isEmpty())
        baseDir = QDir::homePath();
    const QString suggested = QDir(baseDir).filePath(QStringLiteral("qrcode.png"));

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存二维码"), suggested,
        QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty())
        return;

    // Windows 上用户可能不写扩展名；QImage::save 按扩展名选格式，没有扩展名会失败
    const QString target = path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
                               ? path
                               : path + QStringLiteral(".png");
    if (m_renderedImage.save(target, "PNG"))
        setCreateStatus(QStringLiteral("已保存到 %1").arg(QDir::toNativeSeparators(target)),
                        StatusKind::Ok);
    else
        setCreateStatus(QStringLiteral("保存失败：%1").arg(QDir::toNativeSeparators(target)),
                        StatusKind::Error);
}

void QrCodePanel::copyImage()
{
    if (m_renderedImage.isNull())
        return;
    QGuiApplication::clipboard()->setImage(m_renderedImage);
    setCreateStatus(QStringLiteral("二维码图片已复制到剪贴板"), StatusKind::Ok);
}

// ---------------------------------------------------------------------------
// 识别
// ---------------------------------------------------------------------------

void QrCodePanel::chooseImageFile()
{
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (baseDir.isEmpty())
        baseDir = QDir::homePath();

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择二维码图片"), baseDir,
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        setDecodeStatus(QStringLiteral("无法读取这张图片：%1").arg(reader.errorString()),
                        StatusKind::Error);
        return;
    }
    loadImage(image, QFileInfo(path).fileName());
}

void QrCodePanel::pasteImage()
{
    const QImage image = QGuiApplication::clipboard()->image();
    if (image.isNull()) {
        setDecodeStatus(QStringLiteral("剪贴板里没有图片"), StatusKind::Error);
        return;
    }
    loadImage(image, QStringLiteral("剪贴板"));
}

void QrCodePanel::clearDecoded()
{
    m_sourceImage = QImage();
    m_decodedText.clear();
    m_result->clear();
    m_dropZone->setPixmap(QPixmap());
    m_dropZone->setText(QStringLiteral("点击选择图片\n或把图片拖进来\n（Ctrl+V 粘贴剪贴板图片）"));
    setDecodeStatus(QString(), StatusKind::Hint);
}

void QrCodePanel::updateThumbnail()
{
    // 没有图就直接返回：QLabel::setPixmap 会连文字一起清掉，
    // 在这里"顺手清空"会把空状态的提示文字抹掉（清空逻辑在 clearDecoded 里）。
    if (m_sourceImage.isNull())
        return;
    // 投放区会随面板高度变，缩略图跟着重算，才能两边都填满又不溢出。
    const QSize box = m_dropZone->size() - QSize(20, 20);
    if (box.width() <= 0 || box.height() <= 0)
        return;
    // 不放大：小图按原尺寸显示才看得出它到底有多糊，放大只会掩盖问题
    m_dropZone->setPixmap(fitPixmap(m_sourceImage, box.boundedTo(m_sourceImage.size())));
}

void QrCodePanel::loadImage(const QImage &image, const QString &source)
{
    if (image.isNull())
        return;

    m_sourceImage = image;
    m_decodedText.clear();
    m_result->clear();

    // 切到识别页：拖放到生成页时用户预期马上看到结果
    switchPage(1);

    m_dropZone->setText(QString());
    updateThumbnail();
    m_dropZone->setToolTip(QStringLiteral("%1 · %2×%3").arg(source).arg(image.width()).arg(image.height()));

    setDecodeStatus(QStringLiteral("正在识别…"), StatusKind::Hint);
    // 识别是同步的（毫秒级），这里直接算；设置等待光标是为了大图时也有反馈
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const qrcode::DecodeResult result = qrcode::decodeImage(image);
    QApplication::restoreOverrideCursor();

    if (!result.ok) {
        setDecodeStatus(result.error, StatusKind::Error);
        return;
    }

    m_decodedText = result.text;
    m_result->setPlainText(result.text);

    QString status = QStringLiteral("版本 v%1 · 容错 %2 · 掩码 %3 · ")
                         .arg(result.version)
                         .arg(result.eccName)
                         .arg(result.mask);
    if (result.correctedCodewords > 0)
        status += QStringLiteral("纠错修正了 %1 个码字 · ").arg(result.correctedCodewords);
    else
        status += QStringLiteral("图片完好，无需纠错 · ");
    status += QStringLiteral("来自%1").arg(source);
    setDecodeStatus(status, StatusKind::Ok);
}

void QrCodePanel::copyDecodedText()
{
    if (m_decodedText.isEmpty()) {
        setDecodeStatus(QStringLiteral("还没有可复制的内容"), StatusKind::Hint);
        return;
    }
    QGuiApplication::clipboard()->setText(m_decodedText);
    setDecodeStatus(QStringLiteral("内容已复制到剪贴板"), StatusKind::Ok);
}

// ---------------------------------------------------------------------------
// 交互与状态
// ---------------------------------------------------------------------------

void QrCodePanel::switchPage(int index)
{
    if (!m_pages || index < 0 || index >= m_pages->count())
        return;
    m_pages->setCurrentIndex(index);
    m_createTab->setChecked(index == 0);
    m_decodeTab->setChecked(index == 1);
}

void QrCodePanel::setCreateStatus(const QString &text, StatusKind kind)
{
    xtools::theme::Color token = palette::hint;
    if (kind == StatusKind::Ok)
        token = palette::ok;
    else if (kind == StatusKind::Error)
        token = palette::error;
    applyStatus(m_createStatus, text, token);
}

void QrCodePanel::setDecodeStatus(const QString &text, StatusKind kind)
{
    xtools::theme::Color token = palette::hint;
    if (kind == StatusKind::Ok)
        token = palette::ok;
    else if (kind == StatusKind::Error)
        token = palette::error;
    applyStatus(m_decodeStatus, text, token);
}

bool QrCodePanel::eventFilter(QObject *watched, QEvent *event)
{
    // 投放区本质是 QLabel，没有 clicked 信号；这里补一个"按下并抬起"的点击判定
    if (watched == m_dropZone && event->type() == QEvent::MouseButtonRelease) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton
            && m_dropZone->rect().contains(mouseEvent->position().toPoint())) {
            chooseImageFile();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QrCodePanel::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mime = event->mimeData();
    const bool acceptable = mime
        && (mime->hasImage() || (mime->hasUrls() && !mime->urls().isEmpty()));
    if (acceptable) {
        event->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void QrCodePanel::dropEvent(QDropEvent *event)
{
    const QImage image = imageFromMime(event->mimeData());
    if (image.isNull()) {
        setDecodeStatus(QStringLiteral("拖进来的不是可读取的图片"), StatusKind::Error);
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    loadImage(image, QStringLiteral("拖放"));
}

void QrCodePanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 投放区高度随窗口变，缩略图得重新贴合，否则缩放窗口后会留一圈空框
    updateThumbnail();
}

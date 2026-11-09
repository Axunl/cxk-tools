#include "Md5Tool.h"

#include "Md5Logic.h"        // 纯摘要逻辑（UTF-8 字节、16 位截断、HMAC-MD5、比对）
#include "ThemePalette.h"     // 颜色令牌（宿主与插件共用）

#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace {

/// 状态文字配色，集中定义避免各处写字面量漂移（与 JwtPanel 同一套色板）。
/// 用语义令牌而不是色值：深色主题下色值要换一套，令牌名不变。
namespace color {
constexpr auto error   = xtools::theme::Color::Danger;      // 读取失败、比对不一致
constexpr auto success = xtools::theme::Color::Success;     // 比对一致、计算完成
constexpr auto hint    = xtools::theme::Color::TextFaint;   // 空闲提示、次要说明
constexpr auto ink     = xtools::theme::Color::TextBody;    // 常规信息
} // namespace color

/// 结果卡片的底色与描边。卡片只用来把"结果"和"输入"分开，
/// 所以底色比主表面略灰一档，靠 1px 描边收边。
constexpr const char *kCardStyle =
    "QFrame#md5ResultCard{background:@bg-alt;border:1px solid @border;border-radius:8px;}";

QFont fixedFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

/// 小节标题（"待计算文本" / "对比校验" 等），统一灰蓝色小字号。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    return label;
}

/// 次要说明文字（字节统计、已选文件的体积时间等）。
QLabel *makeHintLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@text-faint;"));
    return label;
}

/// 状态行统一外观：允许换行，避免长路径 / 长错误把面板撑宽。
void styleStatus(QLabel *label)
{
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("font-size:12px;"));
}

void setStatus(QLabel *label, const QString &text, xtools::theme::Color token)
{
    // 模板里用 @令牌，换肤时宿主会按当前主题重算颜色（见 ThemePalette.h）
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@%1;")
                                         .arg(xtools::theme::tokenName(token)));
    label->setText(text);
}

/// 人读的体积：1536 → "1.5 KB"。摘要工具里文件可能很大，不显示精确字节反而更好读。
QString formatSize(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    constexpr const char *units[] = {"KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = -1;
    do {
        value /= 1024.0;
        ++unit;
    } while (value >= 1024.0 && unit < 3);
    return QStringLiteral("%1 %2").arg(QString::number(value, 'f', value < 10.0 ? 1 : 0),
                                       QLatin1String(units[unit]));
}

/// 耗时：不足 1 秒按毫秒显示，避免所有小文件都写"0.00 秒"。
QString formatDuration(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 毫秒").arg(ms);
    return QStringLiteral("%1 秒").arg(QString::number(ms / 1000.0, 'f', 2));
}

/// 拖放区的两套描边：默认灰虚线，拖拽悬停时变蓝。
/// 用整段 stylesheet 切换而不是改属性 + repolish，是因为只有两个状态，直接换更省心。
QString dropZoneStyle(bool active)
{
    return active ? QStringLiteral("QFrame#md5DropZone{background:@drop-active-bg;"
                                   "border:1px dashed @drop-active-border;border-radius:10px;}")
                  : QStringLiteral("QFrame#md5DropZone{background:@bg-alt;"
                                   "border:1px dashed @scrollbar;border-radius:10px;}");
}

///
/// 文件拖放区：拖入文件、或点一下打开文件选择框。
///
/// 为什么用 std::function 回调而不是自定义信号：这样就不用给这个内部类加 Q_OBJECT，
/// 少一处 moc 依赖（本文件末尾的 .moc 只需要管面板类）。
///
class DropZone : public QFrame
{
public:
    explicit DropZone(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("md5DropZone"));
        setFixedHeight(96);
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
        xtools::theme::applyStyle(this, dropZoneStyle(false));

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 12, 16, 12);
        layout->setSpacing(4);
        layout->setAlignment(Qt::AlignCenter);

        auto *title = new QLabel(QStringLiteral("拖入文件到这里"), this);
        title->setAlignment(Qt::AlignCenter);
        xtools::theme::applyStyle(title, QStringLiteral("font-size:13px;font-weight:600;color:@text-body;"));
        title->setAttribute(Qt::WA_TransparentForMouseEvents);   // 点击穿透到拖放区

        auto *hint = new QLabel(QStringLiteral("或点击选择文件 · 只读取字节，不会修改文件"), this);
        hint->setAlignment(Qt::AlignCenter);
        xtools::theme::applyStyle(hint, QStringLiteral("font-size:12px;color:@text-faint;"));
        hint->setAttribute(Qt::WA_TransparentForMouseEvents);

        layout->addWidget(title);
        layout->addWidget(hint);
    }

    /// 拖入本地文件时回调（只取第一个）；路径是否可用由面板判断。
    std::function<void(const QString &)> onFile;
    /// 单击拖放区时回调（打开文件选择框）。
    std::function<void()> onClicked;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData() && event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
            xtools::theme::applyStyle(this, dropZoneStyle(true));
        } else {
            QFrame::dragEnterEvent(event);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event->mimeData() && event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            QFrame::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        xtools::theme::applyStyle(this, dropZoneStyle(false));
        QFrame::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        xtools::theme::applyStyle(this, dropZoneStyle(false));
        if (!event->mimeData() || !event->mimeData()->hasUrls())
            return;

        QString path;
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                path = url.toLocalFile();
                break;
            }
        }
        if (path.isEmpty())
            return;

        event->acceptProposedAction();
        if (onFile)
            onFile(path);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        // 只在左键且落点在区域内时触发；拖拽结束的 release 不会走到这里（被 drop 接住）
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            if (onClicked)
                onClicked();
        }
        QFrame::mouseReleaseEvent(event);
    }
};

} // namespace

///
/// 工具面板：「文本」与「文件」两个页签。
///   * 文本：边输边算 32 / 16 位 MD5，可选大写输出、忽略首尾空白，
///     填密钥即时算 HMAC-MD5，粘期望值即时比对；
///   * 文件：拖入或选择文件，分块读取算 MD5，显示体积、修改时间、进度与耗时。
///
/// 所有状态都存在实例里（宿主可能同时开主窗口内嵌与独立窗口两份面板）。
/// 大文件计算走 QTimer 分块，**不用 processEvents**：后者可能在回调里被宿主
/// 销毁面板，接着访问 this 就是野指针；定时器是面板的子对象，面板没了它就没了。
///
class Md5Panel : public QWidget
{
    Q_OBJECT

public:
    explicit Md5Panel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        // 左右 28px 与主窗口页头对齐，视觉上像同一栏
        layout->setContentsMargins(28, 0, 28, 20);
        layout->setSpacing(10);

        m_tabs = new QTabWidget(this);
        m_tabs->setDocumentMode(true);
        layout->addWidget(m_tabs, 1);

        m_tabs->addTab(buildTextTab(), QStringLiteral("文本"));
        m_tabs->addTab(buildFileTab(), QStringLiteral("文件"));

        // 分块读取的定时器：0ms 表示"事件队列空了就跑一次"，界面不会被长任务卡住
        m_hashTimer = new QTimer(this);
        m_hashTimer->setInterval(0);
        connect(m_hashTimer, &QTimer::timeout, this, &Md5Panel::hashNextChunk);

        runTextHash();   // 空输入进入空闲态
        renderFileResult();
    }

private:
    // ------------------------------------------------------------ 文本页

    QWidget *buildTextTab()
    {
        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 12, 0, 0);
        layout->setSpacing(8);

        auto *topRow = new QHBoxLayout;
        topRow->setSpacing(8);
        topRow->addWidget(makeCaption(QStringLiteral("待计算文本"), page));
        topRow->addStretch(1);
        m_trim = new QCheckBox(QStringLiteral("忽略首尾空白"), page);
        m_trim->setToolTip(QStringLiteral(
            "勾选后先去掉文本首尾的空格、制表符与换行再计算。\n"
            "从编辑器复制内容时末尾常带一个换行，是摘要对不上最常见的原因。"));
        topRow->addWidget(m_trim);
        m_upper = new QCheckBox(QStringLiteral("大写输出"), page);
        m_upper->setToolTip(QStringLiteral("勾选后结果用 A-F 大写字母显示（比对时本来就忽略大小写）"));
        topRow->addWidget(m_upper);
        layout->addLayout(topRow);

        m_textInput = new QPlainTextEdit(page);
        m_textInput->setPlaceholderText(
            QStringLiteral("输入或粘贴文本，边输边算（支持多行，按 UTF-8 字节计算）"));
        m_textInput->setMinimumHeight(120);
        layout->addWidget(m_textInput, 1);

        m_textStats = makeHintLabel(page);
        m_textStats->setAlignment(Qt::AlignRight);
        layout->addWidget(m_textStats);

        // 密钥必须有标题：填空时 placeholder 能说明用途，一旦填了内容就只剩一串字符，
        // 用户回头看不知道这行是什么
        layout->addWidget(makeCaption(QStringLiteral("HMAC-MD5 密钥（可选，填了才计算）"), page));
        m_keyInput = new QLineEdit(page);
        m_keyInput->setPlaceholderText(
            QStringLiteral("接口签名里的 key，例如 hmac_md5(key, 串)；留空则 HMAC 一栏为空"));
        layout->addWidget(m_keyInput);

        auto *card = new QFrame(page);
        card->setObjectName(QStringLiteral("md5ResultCard"));
        xtools::theme::applyStyle(card, QString::fromLatin1(kCardStyle));
        auto *grid = new QGridLayout(card);
        grid->setContentsMargins(14, 12, 14, 12);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);

        m_hash32 = addResultRow(grid, 0, QStringLiteral("MD5 · 32 位"), card, &m_copy32);
        m_hash16 = addResultRow(grid, 1, QStringLiteral("MD5 · 16 位"), card, &m_copy16);
        m_hmac   = addResultRow(grid, 2, QStringLiteral("HMAC-MD5"), card, &m_copyHmac);
        grid->setColumnStretch(1, 1);
        layout->addWidget(card);

        layout->addWidget(makeCaption(QStringLiteral("对比校验（粘贴期望值，忽略大小写）"), page));
        auto *compareRow = new QHBoxLayout;
        compareRow->setSpacing(10);
        m_expected = new QLineEdit(page);
        m_expected->setFont(fixedFont());
        m_expected->setPlaceholderText(QStringLiteral("例如 5d41402abc4b2a76b9719d911017c592（32 位或 16 位都认）"));
        compareRow->addWidget(m_expected, 3);
        m_compareStatus = new QLabel(page);
        m_compareStatus->setWordWrap(true);
        compareRow->addWidget(m_compareStatus, 4);
        layout->addLayout(compareRow);

        auto *footnote = makeHintLabel(page);
        footnote->setWordWrap(true);
        footnote->setText(QStringLiteral(
            "16 位 = 32 位结果中间那 16 个字符，不是另一种算法。"
            "MD5 是单向摘要：只能由内容算出摘要，不能从摘要还原内容，也不要用它存密码（该用 bcrypt / Argon2）。"));
        layout->addWidget(footnote);

        // 任何输入变化都重算：MD5 很便宜（几百 MB/s），不需要防抖
        connect(m_textInput, &QPlainTextEdit::textChanged, this, &Md5Panel::runTextHash);
        connect(m_keyInput, &QLineEdit::textChanged, this, &Md5Panel::runTextHash);
        connect(m_expected, &QLineEdit::textChanged, this, &Md5Panel::runTextHash);
        connect(m_trim, &QCheckBox::toggled, this, &Md5Panel::runTextHash);
        connect(m_upper, &QCheckBox::toggled, this, &Md5Panel::runTextHash);
        return page;
    }

    /// 往结果卡里加一行「标题 + 只读结果 + 复制」。
    /// 结果框用等宽字体：摘要是一串十六进制，等宽才能逐位对齐着比对。
    QLineEdit *addResultRow(QGridLayout *grid, int row, const QString &caption,
                            QWidget *parent, QPushButton **copyOut)
    {
        auto *edit = new QLineEdit(parent);
        edit->setReadOnly(true);
        edit->setFont(fixedFont());
        edit->setPlaceholderText(QStringLiteral("—"));

        auto *copy = new QPushButton(QStringLiteral("复制"), parent);
        copy->setFixedWidth(60);

        grid->addWidget(makeCaption(caption, parent), row, 0);
        grid->addWidget(edit, row, 1);
        grid->addWidget(copy, row, 2);

        *copyOut = copy;
        connect(copy, &QPushButton::clicked, this,
                [this, edit, copy] { copyToClipboard(edit->text(), copy); });
        return edit;
    }

    /// 重算文本摘要与比对结果。空输入走空闲态，并把上一次的结果清掉，
    /// 免得屏幕上留着旧摘要被误复制。
    void runTextHash()
    {
        const QString raw = m_textInput->toPlainText();
        const QString text = m_trim->isChecked() ? raw.trimmed() : raw;
        const bool upper = m_upper->isChecked();

        if (text.isEmpty()) {
            m_hash32->clear();
            m_hash16->clear();
            m_hmac->clear();
            if (raw.isEmpty()) {
                m_textStats->clear();
            } else {
                // 只输入了空白又勾了"忽略首尾空白"：说清楚为什么没有结果，
                // 否则用户会以为工具坏了
                m_textStats->setText(QStringLiteral("%1 字节（UTF-8） · 忽略首尾空白后为空")
                                         .arg(raw.toUtf8().size()));
            }
            updateCompare();
            return;
        }

        const QByteArray utf8 = text.toUtf8();
        // QString::size() 是 UTF-16 码元数（emoji 会算 2 个），按码点数才是用户眼里的"字符"
        const int chars = text.toUcs4().size();
        QString stats = QStringLiteral("%1 字符 · %2 字节（UTF-8）").arg(chars).arg(utf8.size());
        if (m_trim->isChecked() && text != raw)
            stats += QStringLiteral(" · 已忽略首尾空白");
        m_textStats->setText(stats);

        const QString hash32 = md5logic::hashBytes(utf8, upper);
        m_hash32->setText(hash32);
        m_hash16->setText(md5logic::shortDigest(hash32));
        // 密钥不做 trim：接口密钥里的空格也是密钥的一部分
        m_hmac->setText(m_keyInput->text().isEmpty()
                            ? QString()
                            : md5logic::hmacMd5(m_keyInput->text().toUtf8(), utf8, upper));

        updateCompare();
    }

    /// 比对期望值。32 位与 16 位都认；不一致时提示最常见的三类坑。
    void updateCompare()
    {
        const QString expected = m_expected->text().trimmed();
        if (expected.isEmpty()) {
            setStatus(m_compareStatus, QStringLiteral("粘贴期望值后自动比对"), color::hint);
            return;
        }
        if (m_hash32->text().isEmpty()) {
            setStatus(m_compareStatus, QStringLiteral("先输入待计算文本"), color::hint);
            return;
        }

        if (md5logic::matches(expected, m_hash32->text())) {
            setStatus(m_compareStatus, QStringLiteral("✓ 与 32 位结果一致"), color::success);
        } else if (md5logic::matches(expected, m_hash16->text())) {
            setStatus(m_compareStatus, QStringLiteral("✓ 与 16 位结果一致"), color::success);
        } else if (expected.size() != 32 && expected.size() != 16) {
            setStatus(m_compareStatus,
                      QStringLiteral("✗ 长度是 %1，MD5 应为 32 位（或 16 位）").arg(expected.size()),
                      color::error);
        } else {
            setStatus(m_compareStatus,
                      QStringLiteral("✗ 不一致：注意末尾换行、首尾空格与文本编码（这里按 UTF-8）"),
                      color::error);
        }
    }

    // ------------------------------------------------------------ 文件页

    QWidget *buildFileTab()
    {
        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 12, 0, 0);
        layout->setSpacing(8);

        auto *drop = new DropZone(page);
        drop->onFile = [this](const QString &path) { startFileHash(path); };
        drop->onClicked = [this] { chooseFile(); };
        layout->addWidget(drop);

        auto *pathRow = new QHBoxLayout;
        pathRow->setSpacing(8);
        m_filePath = new QLineEdit(page);
        m_filePath->setReadOnly(true);
        m_filePath->setPlaceholderText(QStringLiteral("尚未选择文件"));
        pathRow->addWidget(m_filePath, 1);
        m_choose = new QPushButton(QStringLiteral("选择文件…"), page);
        pathRow->addWidget(m_choose);
        layout->addLayout(pathRow);

        m_fileMeta = makeHintLabel(page);
        layout->addWidget(m_fileMeta);

        m_progress = new QProgressBar(page);
        m_progress->setRange(0, 1000);
        m_progress->setTextVisible(false);
        m_progress->setFixedHeight(6);
        xtools::theme::applyStyle(m_progress,
            QStringLiteral("QProgressBar{background:@bg-subtle;border:none;border-radius:3px;}"
                           "QProgressBar::chunk{background:@accent;border-radius:3px;}"));
        m_progress->hide();
        layout->addWidget(m_progress);

        m_fileStatus = new QLabel(page);
        styleStatus(m_fileStatus);
        layout->addWidget(m_fileStatus);

        auto *upperRow = new QHBoxLayout;
        upperRow->setSpacing(8);
        upperRow->addWidget(makeCaption(QStringLiteral("摘要结果"), page));
        upperRow->addStretch(1);
        m_fileUpper = new QCheckBox(QStringLiteral("大写输出"), page);
        upperRow->addWidget(m_fileUpper);
        layout->addLayout(upperRow);

        auto *card = new QFrame(page);
        card->setObjectName(QStringLiteral("md5ResultCard"));
        xtools::theme::applyStyle(card, QString::fromLatin1(kCardStyle));
        auto *grid = new QGridLayout(card);
        grid->setContentsMargins(14, 12, 14, 12);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);
        m_fileHash32 = addResultRow(grid, 0, QStringLiteral("MD5 · 32 位"), card, &m_copyFile32);
        m_fileHash16 = addResultRow(grid, 1, QStringLiteral("MD5 · 16 位"), card, &m_copyFile16);
        grid->setColumnStretch(1, 1);
        layout->addWidget(card);

        auto *footnote = makeHintLabel(page);
        footnote->setWordWrap(true);
        footnote->setText(QStringLiteral(
            "大文件按 8 MB 分块读取，计算过程界面仍可操作；随时可以换文件，旧任务会被取消。"));
        layout->addWidget(footnote);
        layout->addStretch(1);   // 把内容顶在上方，窗口拉高时不会散开

        connect(m_choose, &QPushButton::clicked, this, &Md5Panel::chooseFile);
        connect(m_fileUpper, &QCheckBox::toggled, this, &Md5Panel::renderFileResult);
        return page;
    }

    void chooseFile()
    {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要计算 MD5 的文件"),
                                                          QDir::homePath());
        if (!path.isEmpty())
            startFileHash(path);
    }

    /// 开始（或重新开始）一个文件的计算任务。校验不通过时清空旧结果并给出原因，
    /// 不保留上一次的摘要 —— 那会让人以为新文件算完了。
    void startFileHash(const QString &path)
    {
        m_hashTimer->stop();
        if (m_file.isOpen())
            m_file.close();

        m_filePath->setText(QDir::toNativeSeparators(path));
        m_filePath->setToolTip(QDir::toNativeSeparators(path));
        m_filePath->setCursorPosition(0);   // 长路径默认看到开头，而不是被挤到末尾

        const QFileInfo info(path);
        if (!info.exists()) {
            resetFileState(QStringLiteral("文件不存在：%1").arg(path), color::error);
            return;
        }
        if (info.isDir()) {
            resetFileState(QStringLiteral("这是一个文件夹，请拖入具体文件"), color::error);
            return;
        }
        if (!info.isFile()) {
            resetFileState(QStringLiteral("不是普通文件，无法读取字节"), color::error);
            return;
        }

        m_fileTotal = info.size();
        m_fileHashed = 0;
        m_fileMeta->setText(QStringLiteral("%1 · 修改于 %2").arg(
            formatSize(m_fileTotal), info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));

        m_file.setFileName(info.absoluteFilePath());
        if (!m_file.open(QIODevice::ReadOnly)) {
            resetFileState(QStringLiteral("无法读取该文件：%1").arg(m_file.errorString()), color::error);
            return;
        }

        m_hasher.reset();
        m_fileHex32.clear();
        renderFileResult();
        m_progress->setValue(0);
        m_progress->show();
        setStatus(m_fileStatus, QStringLiteral("正在计算…"), color::ink);
        m_elapsed.start();
        m_hashTimer->start();
    }

    /// 读一块、喂给摘要器，然后让出事件循环；直到文件读完才收尾。
    void hashNextChunk()
    {
        if (!m_file.isOpen()) {
            m_hashTimer->stop();
            return;
        }

        // 8 MB 是吞吐与响应之间的折中：太小则定时器回调过于频繁，太大则单次停顿变长
        constexpr qint64 kChunkSize = 8 * 1024 * 1024;
        const QByteArray chunk = m_file.read(kChunkSize);
        if (!chunk.isEmpty()) {
            m_hasher.addData(chunk);
            m_fileHashed += chunk.size();
            updateFileProgress();
        }

        if (m_file.atEnd()) {
            if (m_file.error() != QFileDevice::NoError) {
                m_hashTimer->stop();
                m_progress->hide();
                resetFileState(QStringLiteral("读取中断：%1").arg(m_file.errorString()), color::error);
                return;
            }
            finishFileHash();
            return;
        }

        // 既没读到数据也没到末尾：设备异常（网络盘掉线等）。不停下就会空转死循环。
        if (chunk.isEmpty()) {
            m_hashTimer->stop();
            m_progress->hide();
            resetFileState(QStringLiteral("读取中断：%1").arg(m_file.errorString()), color::error);
        }
    }

    void updateFileProgress()
    {
        if (m_fileTotal <= 0) {
            setStatus(m_fileStatus, QStringLiteral("正在计算…"), color::ink);
            return;
        }
        // 千分比进度，避免浮点；value/10 得到百分比整数部分
        const int value = static_cast<int>(qMin<qint64>(1000, m_fileHashed * 1000 / m_fileTotal));
        m_progress->setValue(value);
        setStatus(m_fileStatus, QStringLiteral("正在计算… %1%").arg(value / 10), color::ink);
    }

    void finishFileHash()
    {
        m_hashTimer->stop();
        m_file.close();

        // Qt6 的 result() 返回 QByteArrayView，先落到 QByteArray 再转十六进制
        m_fileHex32 = QByteArray(m_hasher.result()).toHex();
        renderFileResult();

        m_progress->setValue(1000);
        m_progress->hide();
        setStatus(m_fileStatus,
                  QStringLiteral("计算完成 · %1 · 用时 %2").arg(formatSize(m_fileTotal),
                                                                formatDuration(m_elapsed.elapsed())),
                  color::success);
    }

    /// 只按 m_fileHex32 重画结果（切换大小写时不需要重新读文件）。
    void renderFileResult()
    {
        const QString hex = m_fileUpper && m_fileUpper->isChecked() ? m_fileHex32.toUpper()
                                                                   : m_fileHex32;
        m_fileHash32->setText(hex);
        m_fileHash16->setText(hex.isEmpty() ? QString() : md5logic::shortDigest(hex));
    }

    /// 失败 / 重来前把结果区清干净，并把状态写清楚。
    void resetFileState(const QString &statusText, xtools::theme::Color token)
    {
        m_fileHex32.clear();
        renderFileResult();
        m_progress->hide();
        m_fileMeta->clear();   // 体积/时间是上一个文件的，失败时不能留着
        setStatus(m_fileStatus, statusText, token);
    }

    // ------------------------------------------------------------ 通用

    /// 复制到剪贴板，按钮文案短暂变"已复制"再恢复。
    /// singleShot 带接收者（按钮），面板提前销毁时定时器随之作废，不会砸野指针。
    void copyToClipboard(const QString &text, QPushButton *button)
    {
        if (text.isEmpty() || !button)
            return;   // 没结果时按"复制"不该把剪贴板清空
        QGuiApplication::clipboard()->setText(text);
        button->setText(QStringLiteral("已复制"));
        QTimer::singleShot(1200, button, [button] { button->setText(QStringLiteral("复制")); });
    }

    QTabWidget *m_tabs = nullptr;

    // 文本页
    QCheckBox      *m_trim = nullptr;
    QCheckBox      *m_upper = nullptr;
    QPlainTextEdit *m_textInput = nullptr;
    QLabel         *m_textStats = nullptr;
    QLineEdit      *m_keyInput = nullptr;
    QLineEdit      *m_hash32 = nullptr;
    QLineEdit      *m_hash16 = nullptr;
    QLineEdit      *m_hmac = nullptr;
    QPushButton    *m_copy32 = nullptr;
    QPushButton    *m_copy16 = nullptr;
    QPushButton    *m_copyHmac = nullptr;
    QLineEdit      *m_expected = nullptr;
    QLabel         *m_compareStatus = nullptr;

    // 文件页
    QLineEdit      *m_filePath = nullptr;
    QPushButton    *m_choose = nullptr;
    QLabel         *m_fileMeta = nullptr;
    QProgressBar   *m_progress = nullptr;
    QLabel         *m_fileStatus = nullptr;
    QCheckBox      *m_fileUpper = nullptr;
    QLineEdit      *m_fileHash32 = nullptr;
    QLineEdit      *m_fileHash16 = nullptr;
    QPushButton    *m_copyFile32 = nullptr;
    QPushButton    *m_copyFile16 = nullptr;

    // 文件计算状态：QFile 与摘要器常驻，避免每块都构造
    QFile              m_file;
    QCryptographicHash m_hasher{QCryptographicHash::Md5};
    QTimer            *m_hashTimer = nullptr;
    QElapsedTimer      m_elapsed;
    qint64             m_fileTotal = 0;
    qint64             m_fileHashed = 0;
    QString            m_fileHex32;   ///< 已完成结果的完整 32 位（小写），切换大小写时复用
};

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo Md5Plugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("md5");
    info.name = QStringLiteral("MD5 摘要");
    // "加密"是用户最常用的说法，虽然 MD5 是摘要不是加密 —— 关键词要跟着用户的叫法走
    info.keywords = QStringList{
        QStringLiteral("md5"), QStringLiteral("hash"), QStringLiteral("摘要"),
        QStringLiteral("哈希"), QStringLiteral("加密"), QStringLiteral("校验"),
        QStringLiteral("文件校验"), QStringLiteral("checksum"), QStringLiteral("hmac"),
        QStringLiteral("签名"), QStringLiteral("16位"), QStringLiteral("32位")};
    info.description = QStringLiteral("文本 / 文件的 MD5 与 HMAC-MD5 计算、比对，完全离线");
    return info;
}

QWidget *Md5Plugin::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存，输入与已选文件都不留存
    return new Md5Panel(parent);
}

// .cpp 里有带 Q_OBJECT 的类（Md5Panel），需要 moc 产物
#include "Md5Tool.moc"

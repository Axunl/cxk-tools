#include "JsonEditor.h"

#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用）

#include "JsonHighlighter.h"
#include "JsonText.h"   // 纯逻辑：字符串边界、前缀扫描、键收集、折叠区扫描

#include <QAbstractItemView>
#include <QColor>
#include <QCompleter>
#include <QFontMetrics>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>

#include <utility>   // std::as_const

namespace {

/// 行号栏的尺寸常量（逻辑像素）。行号与箭头都是手工摆的，
/// 用常量集中起来，改宽度时只改一处。
constexpr int kGutterPadLeft = 10;    ///< 左内边距（行号与栏左边线的距离）
constexpr int kGutterPadRight = 4;    ///< 右内边距（箭头与栏右边线的距离）
constexpr int kNumberGap = 5;         ///< 行号与折叠箭头之间
constexpr int kChevronBox = 14;       ///< 折叠箭头的绘制与点击区域边长
constexpr int kGutterTextGap = 8;     ///< 分隔线到正文之间的留白
constexpr int kFoldDebounceMs = 200;  ///< 输入停下来多久后重算折叠区

/// 敲下这些字符后需要重算补全候选。
///
/// 只认"能构成键名"的字符与引号：空格、逗号、冒号、括号都不能改变当前前缀，
/// 每一个键都去刷一次弹窗反而会让它闪。
bool isCompletionTrigger(QChar ch)
{
    return ch == QLatin1Char('"') || ch.isLetterOrNumber() || ch == QLatin1Char('_')
        || ch == QLatin1Char('-') || ch == QLatin1Char('.');
}

/// 一行开头的空白（缩进）。
QString leadingWhitespace(const QString &text)
{
    int i = 0;
    while (i < text.size()
           && (text.at(i) == QLatin1Char(' ') || text.at(i) == QLatin1Char('\t'))) {
        ++i;
    }
    return text.left(i);
}

/// 画折叠箭头：展开时朝下（⌄），折叠时朝右（›）。
///
/// 用路径而不是 "⌄"/"›" 字符：这两个符号在不同字体里的宽度与基线差别很大，
/// 位置会偏，而且字体缺字时会变成方框。
void drawChevron(QPainter &painter, const QRect &box, bool expanded, const QColor &color)
{
    const QPointF center = QRectF(box).center();
    const qreal halfW = 4.0;
    const qreal halfH = 2.4;

    QPainterPath path;
    if (expanded) {
        path.moveTo(center.x() - halfW, center.y() - halfH);
        path.lineTo(center.x(), center.y() + halfH);
        path.lineTo(center.x() + halfW, center.y() - halfH);
    } else {
        path.moveTo(center.x() - halfH, center.y() - halfW);
        path.lineTo(center.x() + halfH, center.y());
        path.lineTo(center.x() - halfH, center.y() + halfW);
    }

    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

} // namespace

/// 行号栏：只负责"把绘制与点击转发给编辑器"。
///
/// 之所以单独做成子控件而不是在编辑器上画画：它需要在竖向滚动时跟着滚
/// （QWidget::scroll），还得有自己的鼠标命中区域，做成独立控件最省事。
/// 没有 Q_OBJECT 也不需要：它不定义信号槽，只调编辑器的私有钩子（friend）。
///
/// 注意这个类**不能**放进上面的匿名命名空间：头文件里的 `friend class JsonGutter;`
/// 声明的是全局作用域的那个类，匿名命名空间里的同名类是完全不同的类型，友元不生效。
class JsonGutter : public QWidget
{
public:
    explicit JsonGutter(JsonEditor *editor)
        : QWidget(editor)
        , m_editor(editor)
    {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->gutterPaintEvent(event); }
    void mousePressEvent(QMouseEvent *event) override { m_editor->gutterMousePressEvent(event); }
    void mouseMoveEvent(QMouseEvent *event) override { m_editor->gutterMouseMoveEvent(event); }
    void leaveEvent(QEvent *event) override
    {
        m_editor->gutterLeaveEvent();
        QWidget::leaveEvent(event);
    }

private:
    JsonEditor *m_editor = nullptr;
};

JsonEditor::JsonEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    m_highlighter = new JsonHighlighter(document());

    // ---- 补全弹窗
    m_completionModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completionModel, this);
    // setWidget 不只是"告诉 completer 谁是编辑框"：弹窗弹出后键盘焦点在弹窗上，
    // QCompleter 会把按键转发给这个 widget，缺了它打一个字弹窗就收不到后续输入。
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    // 保持"按首次出现顺序"：常用键通常就在文档开头，按字母重排反而不顺手
    m_completer->setModelSorting(QCompleter::UnsortedModel);
    // popup() 是惰性创建的子控件；给它一个 objectName 让宿主样式表能命中它
    // （否则弹窗是系统默认外观，跟浅色卡片界面完全不搭）。
    m_completer->popup()->setObjectName(QStringLiteral("jsonCompleterPopup"));

    // activated 有两个重载（文本 / 模型索引），必须指明取文本那个
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this,
            &JsonEditor::insertCompletion);

    // ---- 候选缓存：文本变动后延迟重建，避免整段扫描跟着按键频率跑
    m_keyTimer = new QTimer(this);
    m_keyTimer->setSingleShot(true);
    m_keyTimer->setInterval(250);
    connect(m_keyTimer, &QTimer::timeout, this, &JsonEditor::rebuildKeys);

    // ---- 折叠区缓存：文本一变就得重算括号配对，同样等用户停手再算
    m_foldTimer = new QTimer(this);
    m_foldTimer->setSingleShot(true);
    m_foldTimer->setInterval(kFoldDebounceMs);
    connect(m_foldTimer, &QTimer::timeout, this, &JsonEditor::revalidateFolds);

    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        // 折叠靠改块可见性实现，那也会触发 textChanged —— 那不是用户编辑，
        // 当成编辑会无限重算（改可见性 → 重算 → 再改可见性）。
        if (m_inFoldUpdate)
            return;
        m_keysDirty = true;
        m_keyTimer->start();
        if (m_errorLine >= 0)
            setErrorLine(-1);   // 内容一变，旧的行号就指错地方了
        m_foldDirty = true;
        m_foldTimer->start();
    });

    // 光标跑进被折叠的行（撤销、跳到出错位置、程序化设光标）就展开它，
    // 否则光标藏在看不见的地方，用户接着打字会打在"空处"。
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!m_foldStarts.isEmpty() && !textCursor().block().isVisible()) {
            removeFoldContaining(textCursor().blockNumber());
            applyFoldVisibility();
        }
        if (m_gutter)
            m_gutter->update();   // 当前行的行号用深色，跟着光标走
    });

    // ---- 行号 / 折叠栏
    m_gutter = new JsonGutter(this);
    // objectName 只为可定位（qss 不按它命中；临时测试与调试时用它找控件）
    m_gutter->setObjectName(QStringLiteral("jsonGutter"));
    setViewportMargins(gutterWidth() + kGutterTextGap, 0, 0, 0);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &JsonEditor::updateGutterWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &JsonEditor::updateGutter);
}

void JsonEditor::setCompletionEnabled(bool on)
{
    m_completionEnabled = on;
    if (!on)
        m_completer->popup()->hide();
}

void JsonEditor::setErrorLine(int line)
{
    m_errorLine = line;
    // 出错行正好被折起来时先展开：标红的行看不见等于没标
    if (line >= 0 && line < document()->blockCount()
        && !document()->findBlockByNumber(line).isVisible()) {
        removeFoldContaining(line);
        applyFoldVisibility();
    }
    applyErrorSelection();
}

void JsonEditor::applyErrorSelection()
{
    QList<QTextEdit::ExtraSelection> selections;
    if (m_errorLine >= 0 && m_errorLine < document()->blockCount()) {
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(
            xtools::theme::color(xtools::theme::Color::JsonErrorBg));
        // FullWidthSelection：整行铺满，而不是只涂有字的宽度
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = QTextCursor(document()->findBlockByNumber(m_errorLine));
        selection.cursor.clearSelection();
        selections.append(selection);
    }
    setExtraSelections(selections);
}

QString JsonEditor::fullText() const
{
    // 逐块拼，而不是 toPlainText()：隐藏块是否计入其中没有文档保证
    // （Qt 6.8 实测会计入，但换版本/平台不保证）。折叠只是显示状态，
    // 格式化与复制要拿到的必须是完整内容。
    QString text;
    const QTextDocument *doc = document();
    text.reserve(doc->characterCount());
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        if (block.blockNumber() > 0)
            text += QLatin1Char('\n');
        text += block.text();
    }
    return text;
}

// ---------------------------------------------------------------------------
// 行号栏
// ---------------------------------------------------------------------------

int JsonEditor::gutterWidth() const
{
    // 按位数留宽，最少按 2 位：只有 1 行时也给双位数留位，
    // 否则文档长到两位数的瞬间整块文本会横向跳一下。
    const int digits = qMax(2, QString::number(qMax(1, blockCount())).size());
    return kGutterPadLeft + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits
        + kNumberGap + kChevronBox + kGutterPadRight;
}

void JsonEditor::layoutGutter()
{
    if (!m_gutter)
        return;
    // 用 contentsRect 定位：qss 的 border + padding 都已经吃在它里面，
    // 直接按 contentsRect 摆才和正文的起始位置对齐（frameWidth() 会把 padding 一起算进去）。
    const QRect content = contentsRect();
    m_gutter->setGeometry(content.left(), content.top(), gutterWidth(), content.height());
}

void JsonEditor::updateGutterWidth()
{
    // 视口左边距比行号栏本身多 kGutterTextGap：那 8px 是分隔线到正文之间的留白。
    // 不留的话正文会紧贴分隔线，读起来像被截断。
    const int margin = gutterWidth() + kGutterTextGap;
    if (viewportMargins().left() != margin)
        setViewportMargins(margin, 0, 0, 0);
    layoutGutter();
}

void JsonEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updateGutterWidth();
    layoutGutter();
}

void JsonEditor::updateGutter(const QRect &rect, int dy)
{
    if (!m_gutter)
        return;
    if (dy != 0)
        m_gutter->scroll(0, dy);   // 竖向滚动直接平移，不必重画整栏
    else
        m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
    // 视口整块重绘（换行导致块数变化、样式变化）时行号栏宽度可能也要跟着变
    if (rect.contains(viewport()->rect()))
        updateGutterWidth();
}

void JsonEditor::gutterPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_gutter);
    // 底色比正文略灰：既能和工具栏的主表面分开，又不至于抢视线
    painter.fillRect(event->rect(), xtools::theme::color(xtools::theme::Color::GutterBg));
    painter.setPen(xtools::theme::color(xtools::theme::Color::BorderSubtle));
    painter.drawLine(m_gutter->width() - 1, event->rect().top(),
                     m_gutter->width() - 1, event->rect().bottom());

    const int numberRight = m_gutter->width() - kGutterPadRight - kChevronBox - kNumberGap;
    const int currentLine = textCursor().blockNumber();

    QTextBlock block = firstVisibleBlock();
    int line = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top() && bottom > top) {
            painter.setPen(line == currentLine
                               ? xtools::theme::color(xtools::theme::Color::GutterCurrent)
                               : xtools::theme::color(xtools::theme::Color::GutterText));
            painter.drawText(QRect(kGutterPadLeft, top, numberRight - kGutterPadLeft, bottom - top),
                             Qt::AlignRight | Qt::AlignVCenter, QString::number(line + 1));

            const int end = foldEnd(line);
            if (end > line) {
                const QRect box(m_gutter->width() - kGutterPadRight - kChevronBox,
                                top + (bottom - top - kChevronBox) / 2, kChevronBox, kChevronBox);
                const bool hovered = (line == m_hoverLine);
                if (hovered) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(xtools::theme::color(xtools::theme::Color::GutterHoverBg));
                    painter.drawRoundedRect(QRectF(box), 4, 4);
                }
                drawChevron(painter, box, !m_foldStarts.contains(line),
                            hovered ? xtools::theme::color(xtools::theme::Color::AccentText)
                                    : xtools::theme::color(xtools::theme::Color::TextFaint));
            }
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++line;
    }
}

void JsonEditor::gutterMousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->position().toPoint();
    // 只有点在那 14px 的箭头框里才算折叠：行号本身不响应点击
    // （点行号选中整行之类的行为没必要，反而会和"把光标点到某行"打架）
    if (pos.x() < m_gutter->width() - kGutterPadRight - kChevronBox)
        return;

    QTextBlock block = firstVisibleBlock();
    int line = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid()) {
        const int bottom = top + qRound(blockBoundingRect(block).height());
        if (block.isVisible() && bottom > top && pos.y() >= top && pos.y() < bottom) {
            toggleFold(line);
            return;
        }
        if (top > m_gutter->height())
            break;
        top = bottom;
        ++line;
        block = block.next();
    }
}

void JsonEditor::gutterMouseMoveEvent(QMouseEvent *event)
{
    // 悬停行只影响那个小箭头的高亮，但它要求逐块累加高度来定位 —— 所以只在
    // 真正换了行时才重画，别让鼠标一动就整栏重绘。
    int hovered = -1;
    const QPoint pos = event->position().toPoint();
    if (pos.x() >= m_gutter->width() - kGutterPadRight - kChevronBox) {
        QTextBlock block = firstVisibleBlock();
        int line = block.blockNumber();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        while (block.isValid()) {
            const int bottom = top + qRound(blockBoundingRect(block).height());
            if (block.isVisible() && bottom > top && pos.y() >= top && pos.y() < bottom) {
                if (foldEnd(line) > line)
                    hovered = line;
                break;
            }
            if (top > m_gutter->height())
                break;
            top = bottom;
            ++line;
            block = block.next();
        }
    }

    if (hovered == m_hoverLine)
        return;
    m_hoverLine = hovered;
    m_gutter->setCursor(hovered >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    m_gutter->update();
}

void JsonEditor::gutterLeaveEvent()
{
    if (m_hoverLine < 0)
        return;
    m_hoverLine = -1;
    m_gutter->update();
}

// ---------------------------------------------------------------------------
// 折叠
// ---------------------------------------------------------------------------

int JsonEditor::foldEnd(int line) const
{
    // 区域信息过期时一律当作"不可折叠"：宁可晚 200ms 才出现箭头，
    // 也不要在旧行号上画箭头，那会指到别的区块去。
    if (m_foldDirty)
        return -1;
    return m_regionEnd.value(line, -1);
}

void JsonEditor::ensureFoldRegions()
{
    if (!m_foldDirty)
        return;

    QStringList lines;
    const QTextDocument *doc = document();
    lines.reserve(doc->blockCount());
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
        lines.append(block.text());

    m_regionEnd.clear();
    const QVector<jsontext::FoldRegion> regions = jsontext::foldRegions(lines);
    for (const jsontext::FoldRegion &region : regions) {
        // 一行开两层括号（`[{`）时两个区块的起点相同：取更靠后的终点，
        // 外层覆盖内层，折叠后隐藏的行数才是对的。
        const int existing = m_regionEnd.value(region.startLine, -1);
        if (region.endLine > existing)
            m_regionEnd.insert(region.startLine, region.endLine);
    }
    m_foldDirty = false;
}

bool JsonEditor::applyFoldVisibility()
{
    ensureFoldRegions();

    bool changed = false;
    m_inFoldUpdate = true;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const int line = block.blockNumber();
        bool hide = false;
        for (auto it = m_foldStarts.cbegin(); it != m_foldStarts.cend(); ++it) {
            if (line > it.key() && line <= it.value()) {
                hide = true;
                break;
            }
        }
        // 只在状态真的变了时才 setVisible：它会引起重排，
        // 无脑全量设置会让每次点击都白抖一下。
        if (block.isVisible() == hide) {
            block.setVisible(!hide);
            changed = true;
        }
    }
    m_inFoldUpdate = false;

    if (changed) {
        viewport()->update();
        if (m_gutter)
            m_gutter->update();
    }
    return changed;
}

void JsonEditor::removeFoldContaining(int line)
{
    for (auto it = m_foldStarts.cbegin(); it != m_foldStarts.cend(); ++it) {
        if (line > it.key() && line <= it.value()) {
            m_foldStarts.remove(it.key());
            return;
        }
    }
}

bool JsonEditor::toggleFold(int line)
{
    ensureFoldRegions();
    const int end = foldEnd(line);
    if (end <= line)
        return false;

    if (m_foldStarts.contains(line)) {
        m_foldStarts.remove(line);
    } else {
        m_foldStarts.insert(line, end);
        // 光标在被折起来的区块里：先挪到折叠行末尾，否则光标被藏起来，
        // 用户接着敲的字会落进看不见的地方。
        const int cursorLine = textCursor().blockNumber();
        if (cursorLine > line && cursorLine <= end) {
            QTextCursor cursor(document()->findBlockByNumber(line));
            cursor.movePosition(QTextCursor::EndOfBlock);
            setTextCursor(cursor);
        }
    }

    applyFoldVisibility();
    viewport()->update();   // 占位符（"⋯ N 行"）要跟着折叠状态变
    if (m_gutter)
        m_gutter->update();
    return true;
}

void JsonEditor::revalidateFolds()
{
    // 文本变过，块号可能已经漂移：重算区域，再按"形状是否还对得上"筛一遍折叠状态。
    m_foldDirty = true;
    ensureFoldRegions();

    for (auto it = m_foldStarts.begin(); it != m_foldStarts.end();) {
        // 起点行还在开括号、终点行也一样，才认为还是同一块；
        // 对不上就丢掉 —— 沿用旧行号会把另一段内容莫名藏起来。
        if (m_regionEnd.value(it.key(), -1) != it.value())
            it = m_foldStarts.erase(it);
        else
            ++it;
    }

    if (!textCursor().block().isVisible())
        removeFoldContaining(textCursor().blockNumber());

    applyFoldVisibility();
    viewport()->update();
    if (m_gutter)
        m_gutter->update();
}

void JsonEditor::paintFoldChips()
{
    m_chipRects.clear();
    m_chipLines.clear();
    if (m_foldStarts.isEmpty() || m_foldDirty)
        return;

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 比正文小一号：它是"这里藏了东西"的注记，不该和正文抢注意力
    QFont chipFont = font();
    if (chipFont.pointSizeF() > 0)
        chipFont.setPointSizeF(chipFont.pointSizeF() * 0.86);
    painter.setFont(chipFont);
    const QFontMetrics metrics(chipFont);

    QTextBlock block = firstVisibleBlock();
    while (block.isValid()) {
        const QRectF blockRect = blockBoundingGeometry(block).translated(contentOffset());
        if (blockRect.top() > viewport()->height())
            break;

        const int line = block.blockNumber();
        if (block.isVisible() && m_foldStarts.contains(line)) {
            const QTextLayout *layout = block.layout();
            const qreal textWidth =
                layout && layout->lineCount() > 0 ? layout->lineAt(0).naturalTextWidth() : 0.0;
            // 隐藏的是 line+1 … end 这些行
            const QString label =
                QStringLiteral("⋯ %1 行").arg(foldEnd(line) - line);
            const int height = qMin(metrics.height() + 2, qMax(14, qRound(blockRect.height()) - 2));
            const QRect chip(qRound(blockRect.left() + textWidth + 8),
                             qRound(blockRect.center().y() - height / 2.0),
                             metrics.horizontalAdvance(label) + 14, height);

            painter.setPen(Qt::NoPen);
            painter.setBrush(xtools::theme::color(xtools::theme::Color::AccentSoft));
            painter.drawRoundedRect(QRectF(chip), 4, 4);
            painter.setPen(xtools::theme::color(xtools::theme::Color::TextMuted));
            painter.drawText(chip, Qt::AlignCenter, label);

            // 点击区比视觉范围略大一圈，好点一些
            m_chipRects.append(chip.adjusted(-2, -2, 2, 2));
            m_chipLines.append(line);
        }
        block = block.next();
    }
}

void JsonEditor::paintEvent(QPaintEvent *event)
{
    QPlainTextEdit::paintEvent(event);
    paintFoldChips();   // 折叠占位符画在正文之上
}

void JsonEditor::mousePressEvent(QMouseEvent *event)
{
    // 点"⋯ N 行"就是展开（比去行号栏找那个小箭头顺手）
    if (event->button() == Qt::LeftButton) {
        const QPoint pos = event->position().toPoint();
        for (int i = 0; i < m_chipRects.size(); ++i) {
            if (m_chipRects.at(i).contains(pos)) {
                toggleFold(m_chipLines.at(i));
                return;
            }
        }
    }
    QPlainTextEdit::mousePressEvent(event);
}

void JsonEditor::mouseMoveEvent(QMouseEvent *event)
{
    QPlainTextEdit::mouseMoveEvent(event);

    const QPoint pos = event->position().toPoint();
    bool onChip = false;
    for (const QRect &chip : std::as_const(m_chipRects)) {
        if (chip.contains(pos)) {
            onChip = true;
            break;
        }
    }
    if (onChip)
        viewport()->setCursor(Qt::PointingHandCursor);
    else if (viewport()->cursor().shape() != Qt::IBeamCursor)
        viewport()->setCursor(Qt::IBeamCursor);
}

void JsonEditor::inputMethodEvent(QInputMethodEvent *event)
{
    // 记下"正在拼字"：preedit 期间的按键不是最终文本，
    // 自动缩进必须让开，否则会把拼字打断（或按错误的上下文算缩进）。
    m_composing = !event->preeditString().isEmpty();
    QPlainTextEdit::inputMethodEvent(event);
}

void JsonEditor::keyPressEvent(QKeyEvent *event)
{
    // 弹窗打开时，Enter / Esc / Tab 归 QCompleter 处理（它会选中或关掉弹窗，
    // 选中后发 activated）。这里必须 ignore 并退出，否则编辑器会先插入换行/制表符。
    if (m_completer->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore();
            return;
        default:
            break;
        }
    }

    // Ctrl+空格：主动唤起补全（前缀为空时列出全部键名）
    if (event->key() == Qt::Key_Space && event->modifiers().testFlag(Qt::ControlModifier)) {
        triggerCompletion(true);
        return;
    }

    // 自动缩进要排在自动补齐前面：行首敲 } 时先退缩进，再决定要不要插字符
    if (handleAutoIndent(event))
        return;

    if (handleAutoPair(event))
        return;

    QPlainTextEdit::keyPressEvent(event);

    if (!m_completionEnabled)
        return;

    const QString typed = event->text();
    if (typed.size() == 1 && isCompletionTrigger(typed.at(0))) {
        triggerCompletion(false);
    } else if (event->key() == Qt::Key_Backspace) {
        triggerCompletion(false);
    } else if (m_completer->popup()->isVisible()) {
        // 方向键、回车之外的其它按键让弹窗收起，避免它停在陈旧的位置上
        m_completer->popup()->hide();
    }
}

bool JsonEditor::handleAutoPair(QKeyEvent *event)
{
    if (isReadOnly() || !m_completionEnabled)
        return false;
    // 带 Ctrl/Alt 的组合键是命令，不该顺手插字符
    if (event->modifiers().testFlag(Qt::ControlModifier)
        || event->modifiers().testFlag(Qt::AltModifier)) {
        return false;
    }

    const QString typed = event->text();
    if (typed.size() != 1)
        return false;
    const QChar ch = typed.at(0);
    if (ch != QLatin1Char('"') && ch != QLatin1Char('{') && ch != QLatin1Char('[')
        && ch != QLatin1Char('}') && ch != QLatin1Char(']')) {
        return false;
    }

    // 只看当前行：前缀、字符串边界、键/值判断都在同一行内成立（JSON 字符串不跨行）。
    // 用全文 toPlainText() 也能做，但每敲一个字符复制一次几 MB 文档，大 JSON 会明显卡顿。
    QTextCursor cursor = textCursor();
    const QTextBlock block = cursor.block();
    const QString line = block.text();
    const int pos = cursor.positionInBlock();
    const bool hasSelection = cursor.hasSelection();

    // 选中内容后敲开符号 = 把选中的内容包起来（比"先删再重写"顺手）
    auto wrapSelection = [this](QChar open, QChar close) {
        QTextCursor c = textCursor();
        const QString selected = c.selectedText();
        c.insertText(QString(open) + selected + QString(close));
        // 重新选中被包住的原文，方便继续操作
        c.setPosition(c.position() - 1 - selected.size());
        c.setPosition(c.position() + selected.size(), QTextCursor::KeepAnchor);
        setTextCursor(c);
    };
    auto insertPair = [this](QChar open, QChar close) {
        QTextCursor c = textCursor();
        c.insertText(QString(open) + QString(close));
        c.movePosition(QTextCursor::Left);   // 光标落到两个符号之间
        setTextCursor(c);
    };
    // 后面已经是同一个闭符号：只把光标挪过去，不再插一个
    auto skipOver = [this] {
        QTextCursor c = textCursor();
        c.movePosition(QTextCursor::Right);
        setTextCursor(c);
    };

    if (ch == QLatin1Char('"')) {
        if (hasSelection) {
            wrapSelection(ch, ch);
            return true;
        }
        // 后面已经有一个引号（多半是自动配对留下的收尾引号）：直接跨过去，
        // 不再插一个，否则会变成 """" 这种多余的引号堆。
        if (pos < line.size() && line.at(pos) == QLatin1Char('"')) {
            skipOver();
            return true;
        }
        // 已经在字符串里 = 这一下是收尾引号，必须原样插入。
        // 否则会插出一对多余的引号（"abc" → "abc""），这是自动配对最常见的坑。
        if (jsontext::insideString(line, pos))
            return false;
        insertPair(ch, ch);
        return true;
    }

    if (ch == QLatin1Char('{') || ch == QLatin1Char('[')) {
        const QChar close = ch == QLatin1Char('{') ? QLatin1Char('}') : QLatin1Char(']');
        if (!hasSelection && pos < line.size() && line.at(pos) == close) {
            skipOver();
            return true;
        }
        if (hasSelection) {
            wrapSelection(ch, close);
            return true;
        }
        insertPair(ch, close);
        return true;
    }

    // 闭符号：只在"后面已经有一个同样的"时跳过，其它情况走默认插入
    if (!hasSelection && pos < line.size() && line.at(pos) == ch) {
        skipOver();
        return true;
    }
    return false;
}

bool JsonEditor::handleAutoIndent(QKeyEvent *event)
{
    if (isReadOnly() || m_composing)
        return false;   // 只读区不动；输入法拼字期间这些按键还不是最终文本
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;

    QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    const int pos = cursor.positionInBlock();
    const QString indent = leadingWhitespace(line);
    // 一级缩进：默认 4 空格（与 QJsonDocument::Indented 一致）；
    // 当前行用 Tab 缩进时跟着用 Tab，免得一份文件里混两种缩进。
    const QString unit = indent.contains(QLatin1Char('\t')) ? QStringLiteral("\t")
                                                            : QStringLiteral("    ");

    // ---- 回车：按当前行的结构决定新行缩进
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const QString before = line.left(pos).trimmed();
        const QString after = line.mid(pos).trimmed();
        const bool opens = !before.isEmpty()
            && (before.endsWith(QLatin1Char('{')) || before.endsWith(QLatin1Char('[')));
        const bool closes = !after.isEmpty()
            && (after.startsWith(QLatin1Char('}')) || after.startsWith(QLatin1Char(']')));

        cursor.beginEditBlock();
        if (opens && closes) {
            // 光标正卡在自动补齐出来的 "{}" / "[]" 中间：一口气拆成三行，
            // 中间那行已经缩进好了，用户不用再敲回车和退格去"抠"出来。
            cursor.insertText(QStringLiteral("\n") + indent + unit + QStringLiteral("\n") + indent);
            cursor.movePosition(QTextCursor::Up);
            cursor.movePosition(QTextCursor::EndOfBlock);
        } else {
            // 上一行是开括号 → 缩进一级；否则与上一行对齐（连续写多个键最省事）
            cursor.insertText(QStringLiteral("\n") + indent + (opens ? unit : QString()));
        }
        cursor.endEditBlock();
        setTextCursor(cursor);
        return true;
    }

    const QString typed = event->text();
    if (typed.size() != 1)
        return false;
    const QChar ch = typed.at(0);
    if (ch != QLatin1Char('}') && ch != QLatin1Char(']'))
        return false;
    if (!line.left(pos).trimmed().isEmpty())
        return false;   // 行里已经有内容：交给默认插入与自动配对去处理

    // ---- 行首敲闭括号（1）：后面紧跟着同一个闭括号就直接跳过去。
    // 这是"回车拆开 {} 后再敲 }"的常见路径 —— 不跳的话会多出一个闭括号。
    {
        int probe = cursor.position();
        const int limit = document()->characterCount();
        int target = -1;
        while (probe < limit) {
            const QChar next = document()->characterAt(probe);
            if (next == ch) {
                target = probe;
                break;
            }
            if (!next.isSpace())
                break;   // 中间夹着别的内容就不猜了，走正常插入
            ++probe;
        }
        if (target >= 0) {
            cursor.beginEditBlock();
            cursor.setPosition(target, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();   // 吃掉中间的空白与换行，闭括号被拉到光标处
            for (int i = 0; i < pos; ++i)
                cursor.deletePreviousChar();   // 再吃掉本行的缩进
            cursor.movePosition(QTextCursor::NextCharacter);
            cursor.endEditBlock();
            setTextCursor(cursor);
            return true;
        }
    }

    // ---- 行首敲闭括号（2）：没有可跳过的闭括号，就先把缩进退一级，
    // 免得 } 孤零零地吊在缩进里。
    int dedent = 0;
    while (dedent < pos && dedent < unit.size()
           && (line.at(pos - 1 - dedent) == QLatin1Char(' ')
               || line.at(pos - 1 - dedent) == QLatin1Char('\t'))) {
        ++dedent;
    }
    if (dedent > 0) {
        cursor.beginEditBlock();
        for (int i = 0; i < dedent; ++i)
            cursor.deletePreviousChar();
        cursor.insertText(QString(ch));
        cursor.endEditBlock();
        setTextCursor(cursor);
        return true;
    }
    return false;
}

void JsonEditor::insertFromMimeData(const QMimeData *source)
{
    QPlainTextEdit::insertFromMimeData(source);
    // 所有粘贴入口（Ctrl+V / 右键菜单 / 拖放 / 中键）都汇到这里，
    // 不用在键盘事件里逐个匹配快捷键，也不会漏掉某一条路径。
    if (source && source->hasText())
        emit textPasted();
}

void JsonEditor::rebuildKeys()
{
    m_keyTimer->stop();
    // 用 fullText()：被折叠的区块里也有键，同样要进候选
    m_keys = jsontext::collectKeys(fullText());
    m_keysDirty = false;
}

void JsonEditor::triggerCompletion(bool force)
{
    if (!m_completionEnabled || isReadOnly())
        return;

    // 只看当前行：前缀、字符串边界、键/值判断都在同一行内成立（JSON 字符串不跨行）。
    // 用全文 toPlainText() 也能算，但每敲一个字符就要复制一次整篇文档，大 JSON 会卡。
    const QTextCursor cursor = textCursor();
    const QTextBlock block = cursor.block();
    const QString line = block.text();
    const int posInBlock = cursor.positionInBlock();

    // 只有"键的位置"才补键名。值字符串里补键名是纯噪音，直接不弹。
    const bool inString = jsontext::insideString(line, posInBlock);
    bool keyContext = false;
    if (inString) {
        const int quote = jsontext::stringStartAt(line, posInBlock);
        keyContext = quote >= 0 && jsontext::inKeyPosition(line, quote);
        if (!keyContext) {
            m_completer->popup()->hide();
            return;
        }
    }

    if (m_keysDirty)
        rebuildKeys();

    const QStringList candidates = keyContext
        ? m_keys
        : QStringList{QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("null")};
    if (candidates.isEmpty()) {
        m_completer->popup()->hide();
        return;
    }

    // 前缀永远在当前行内；换成文档绝对位置是为了插入时能直接定位
    const int prefixStartInBlock = jsontext::completionPrefixStart(line, posInBlock);
    const QString prefix = line.mid(prefixStartInBlock, posInBlock - prefixStartInBlock);

    // 键位置允许空前缀：刚敲下引号就想看看这个文件里有哪些键，是最常见的用法。
    // 其它位置至少要有内容，否则回车后、每打一个空格都弹一次，纯干扰。
    if (prefix.isEmpty() && !force && !keyContext) {
        m_completer->popup()->hide();
        return;
    }

    m_prefixStart = block.position() + prefixStartInBlock;
    m_completionModel->setStringList(candidates);
    m_completer->setCompletionPrefix(prefix);

    // 没有候选，或唯一候选就是已经打完的词：不必弹窗
    if (m_completer->completionCount() == 0
        || (m_completer->completionCount() == 1 && m_completer->currentCompletion() == prefix)) {
        m_completer->popup()->hide();
        return;
    }

    QRect rect = cursorRect();
    rect.setWidth(m_completer->popup()->sizeHintForColumn(0)
                  + m_completer->popup()->verticalScrollBar()->sizeHint().width() + 12);
    m_completer->complete(rect);
    // 必须在 complete() 之后定位当前项：complete() 内部会按新前缀重建候选，
    // 之前设的选中项会被清掉（表现为弹窗里一条都不高亮）。
    // 而且要用 completionModel（弹窗实际显示的代理模型），setCurrentRow()
    // 在这条路径上拿到的是空索引，不会生效。
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
}

void JsonEditor::insertCompletion(const QString &completion)
{
    if (completion.isEmpty())
        return;

    QTextCursor cursor = textCursor();
    const int cursorPos = cursor.position();
    const int start = qBound(0, m_prefixStart, cursorPos);
    const QString line = cursor.block().text();
    const int posInBlock = cursor.positionInBlock();

    QString inserted = completion;
    // 键名补全发生在开引号之后，顺手补上闭引号，省得用户再敲一下；
    // 闭引号已经在光标后就不重复加。
    const bool nextIsQuote = posInBlock < line.size() && line.at(posInBlock) == QLatin1Char('"');
    if (!nextIsQuote && jsontext::insideString(line, posInBlock))
        inserted += QLatin1Char('"');

    cursor.setPosition(start);
    cursor.setPosition(cursorPos, QTextCursor::KeepAnchor);
    cursor.insertText(inserted);
    setTextCursor(cursor);
}

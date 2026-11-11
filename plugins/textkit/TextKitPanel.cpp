#include "TextKitPanel.h"

#include "ThemePalette.h"
#include "LayoutMetrics.h"   // 颜色令牌（宿主与插件共用）

#include "TextDiff.h"    // 纯逻辑：行级 + 字符级差异，可单独 include 做断言测试
#include "TextLines.h"   // 纯逻辑：切行、加引号拼接、集合差

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>   // Qt6 里 QPainterPath 不再由 QPainter 间接引入
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 对比页的防抖间隔。每敲一个字都跑一遍 LCS 会在几千行的输入上明显掉帧，
/// 停手 180ms 后再算，交互上感觉不到延迟，又不会卡顿。
constexpr int kDiffDebounceMs = 180;

/// 差异配色，集中定义避免各处写字面量漂移（语义令牌，浅/深各一套色值）。
namespace palette {
constexpr auto hint = xtools::theme::Color::TextMuted;            // 常规提示
constexpr auto error = xtools::theme::Color::Danger;              // 错误提示
constexpr auto removedBg = xtools::theme::Color::DiffRemovedBg;   // 只在左侧 / 修改行的底色
constexpr auto removedMark = xtools::theme::Color::DiffRemovedMark; // 行内被删掉的字
constexpr auto addedBg = xtools::theme::Color::DiffAddedBg;       // 只在右侧 / 修改行的底色
constexpr auto addedMark = xtools::theme::Color::DiffAddedMark;   // 行内新增的字
} // namespace palette

/// 小节标题（"引号" / "文本 A" 等），统一灰蓝色小字号。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    return label;
}

/// 页头下方的一句话说明：灰色小字、自动折行。
QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@%1;")
                                     .arg(xtools::theme::tokenName(palette::hint)));
    return label;
}

/// 状态行：初始样式与 makeHint 一致，之后由 TextKitPanel::setStatusText 改颜色。
QLabel *makeStatus(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@%1;")
                                     .arg(xtools::theme::tokenName(palette::hint)));
    return label;
}

///
/// 文本编辑框统一外观。
///
/// * 等宽字体：对比两段代码/日志时字符宽度一致，行内高亮才能和原文对齐；
/// * NoWrap：对比页左右两栏靠「同样的行数」在视觉上对齐，一旦折行，
///   左边第 5 行就会和右边第 5 行错位（引号页的结果则单独改成按宽度折行）。
///
QPlainTextEdit *makeEditor(QWidget *parent, bool readOnly)
{
    auto *edit = new QPlainTextEdit(parent);
    edit->setReadOnly(readOnly);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    return edit;
}

/// 下拉框右侧的小三角。
///
/// 全局 qss 给 `QComboBox::drop-down` 写了规则，Qt 因此不再画默认箭头
/// （见 docs/PLUGIN.md 第 4 节）。自绘一个透明子控件最省事，鼠标事件直接穿透给下拉框。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(10, 8);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        // 自绘小三角跟主题走：写死浅灰在深色底上几乎看不见
        painter.setBrush(isEnabled() ? xtools::theme::color(xtools::theme::Color::TextFaint)
                                     : xtools::theme::color(xtools::theme::Color::TextDisabled));
        QPainterPath path;
        path.moveTo(0.5, 2.0);
        path.lineTo(9.5, 2.0);
        path.lineTo(5.0, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
    }
};

/// 自带箭头的下拉框：箭头是它的子控件，跟着 resize 摆到右侧 18px 区域。
/// 不用 eventFilter 是因为箭头属于自己，重写 resizeEvent 就够，省掉一处外部安装。
class ArrowCombo : public QComboBox
{
public:
    explicit ArrowCombo(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        m_arrow = new ComboArrow(this);
        placeArrow();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QComboBox::resizeEvent(event);
        placeArrow();
    }

private:
    void placeArrow()
    {
        // qss 里 padding-right 是 24px、::drop-down 箭头区 18px，摆在同一位置才对得上。
        const QRect box = rect();
        m_arrow->move(box.width() - 18 + 4, (box.height() - m_arrow->height()) / 2);
    }

    ComboArrow *m_arrow = nullptr;
};

///
/// 把差异结果上色到某一侧的编辑框。
///
/// 两级高亮：
///   * 行级 —— 用 QTextFormat::FullWidthSelection 铺满整行，读起来像 diff 工具的行底色；
///   * 行内 —— 只在「修改」行上，把字符级 LCS 算出的区间用更深的底色标出来。
///
/// 用 extraSelections 而不是改 HTML/字符格式：文本本身保持原样，用户能继续编辑、
/// 选中、复制，高亮只是覆盖层，重算时整体替换即可。
void applyDiffHighlight(QPlainTextEdit *edit, const textdiff::Result &result, bool leftSide)
{
    QList<QTextEdit::ExtraSelection> selections;
    QTextDocument *document = edit->document();

    for (const textdiff::Row &row : result.rows) {
        if (row.kind == textdiff::Kind::Equal)
            continue;

        const int lineNumber = leftSide ? row.leftLine : row.rightLine;
        if (lineNumber < 0)
            continue;   // 这一行在另一侧，本侧没有对应行

        const bool touched = row.kind == textdiff::Kind::Changed
                             || (leftSide ? row.kind == textdiff::Kind::Removed
                                          : row.kind == textdiff::Kind::Added);
        if (!touched)
            continue;

        const QTextBlock block = document->findBlockByNumber(lineNumber);
        if (!block.isValid())
            continue;

        QTextEdit::ExtraSelection lineSelection;
        lineSelection.format.setBackground(
            xtools::theme::color(leftSide ? palette::removedBg : palette::addedBg));
        lineSelection.format.setProperty(QTextFormat::FullWidthSelection, true);
        lineSelection.cursor = QTextCursor(block);
        selections.append(lineSelection);

        const QVector<QPair<int, int>> &marks = leftSide ? row.leftMarks : row.rightMarks;
        const int textLength = block.length() - 1;   // 不含段落分隔符
        for (const QPair<int, int> &mark : marks) {
            if (mark.second <= 0 || mark.first < 0 || mark.first + mark.second > textLength)
                continue;   // 文本在算完之后被改过：宁可少标一段，也不要越界取字符
            QTextEdit::ExtraSelection markSelection;
            markSelection.format.setBackground(
                xtools::theme::color(leftSide ? palette::removedMark : palette::addedMark));
            QTextCursor cursor(block);
            cursor.setPosition(block.position() + mark.first);
            cursor.setPosition(block.position() + mark.first + mark.second, QTextCursor::KeepAnchor);
            markSelection.cursor = cursor;
            selections.append(markSelection);
        }
    }
    edit->setExtraSelections(selections);
}

/// 把差异结果压成一份「`-` 旧行 / `+` 新行」的清单，供「复制差异清单」使用。
/// 相同的行不列，避免几千行的文本复制出一大堆无信息内容。
QString buildDiffReport(const textdiff::Result &result)
{
    QStringList lines;
    for (const textdiff::Row &row : result.rows) {
        switch (row.kind) {
        case textdiff::Kind::Equal:
            break;
        case textdiff::Kind::Removed:
            lines.append(QStringLiteral("- ") + row.leftText);
            break;
        case textdiff::Kind::Added:
            lines.append(QStringLiteral("+ ") + row.rightText);
            break;
        case textdiff::Kind::Changed:
            lines.append(QStringLiteral("- ") + row.leftText);
            lines.append(QStringLiteral("+ ") + row.rightText);
            break;
        }
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace

// ---------------------------------------------------------------- 构造

TextKitPanel::TextKitPanel(QWidget *parent)
    : QWidget(parent)
{
    // 防抖定时器要早于页签创建：搭建页面时若 setPlainText 触发了 textChanged，
    // scheduleDiff() 会用到它，晚建就会在构造期空指针。
    m_diffTimer = new QTimer(this);
    m_diffTimer->setSingleShot(true);
    m_diffTimer->setInterval(kDiffDebounceMs);
    connect(m_diffTimer, &QTimer::timeout, this, &TextKitPanel::runDiff);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);   // 与宿主页头 28px 留白对齐
    layout->setSpacing(10);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);   // 与 JWT 插件一致：页签条融进页面，不画外框
    layout->addWidget(m_tabs, 1);

    m_tabs->addTab(buildQuoteTab(), QStringLiteral("加引号"));
    m_tabs->addTab(buildDiffTab(), QStringLiteral("文本对比"));
    m_tabs->addTab(buildSetTab(), QStringLiteral("差异提取"));

    // 空输入下的初始文案与按钮置灰状态
    updateQuote();
    updateSetDiff();
    runDiff();
}

// ---------------------------------------------------------------- 加引号页

QWidget *TextKitPanel::buildQuoteTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(makeHint(
        QStringLiteral("左边每行一项，右边实时给出加引号、并用分隔符连成一行的结果："
                       "1 / 2 / 3 会变成 '1','2','3'，可直接粘进 SQL 的 IN (...) 或代码里的数组。"),
        page));

    auto *options = new QHBoxLayout;
    options->setSpacing(8);
    options->addWidget(makeCaption(QStringLiteral("引号"), page));

    m_quoteMark = new ArrowCombo(page);
    // data 存真实引号字符；空串表示「不加引号」（此时只做连接）
    m_quoteMark->addItem(QStringLiteral("单引号  ' '"), QStringLiteral("'"));
    m_quoteMark->addItem(QStringLiteral("双引号  \" \""), QStringLiteral("\""));
    m_quoteMark->addItem(QStringLiteral("反引号  ` `"), QStringLiteral("`"));
    m_quoteMark->addItem(QStringLiteral("不加引号"), QString());
    m_quoteMark->setMinimumWidth(118);
    options->addWidget(m_quoteMark);

    options->addSpacing(10);
    options->addWidget(makeCaption(QStringLiteral("分隔符"), page));
    m_quoteSeparator = new ArrowCombo(page);
    m_quoteSeparator->addItem(QStringLiteral("逗号  ,"), QStringLiteral(","));
    m_quoteSeparator->addItem(QStringLiteral("逗号 + 空格  , "), QStringLiteral(", "));
    m_quoteSeparator->addItem(QStringLiteral("空格"), QStringLiteral(" "));
    m_quoteSeparator->addItem(QStringLiteral("顿号  、"), QStringLiteral("、"));
    m_quoteSeparator->addItem(QStringLiteral("分号  ;"), QStringLiteral(";"));
    m_quoteSeparator->addItem(QStringLiteral("竖线  |"), QStringLiteral("|"));
    m_quoteSeparator->addItem(QStringLiteral("换行"), QStringLiteral("\n"));
    m_quoteSeparator->setMinimumWidth(134);
    options->addWidget(m_quoteSeparator);

    options->addSpacing(10);
    m_quoteDropEmpty = new QCheckBox(QStringLiteral("去掉空行"), page);
    m_quoteDropEmpty->setChecked(true);
    m_quoteTrim = new QCheckBox(QStringLiteral("去首尾空白"), page);
    m_quoteTrim->setChecked(true);
    m_quoteUnique = new QCheckBox(QStringLiteral("去重"), page);
    options->addWidget(m_quoteDropEmpty);
    options->addWidget(m_quoteTrim);
    options->addWidget(m_quoteUnique);
    options->addStretch(1);
    layout->addLayout(options);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);

    auto *leftColumn = new QVBoxLayout;
    leftColumn->setSpacing(6);
    leftColumn->addWidget(makeCaption(QStringLiteral("输入（每行一项）"), page));
    m_quoteInput = makeEditor(page, false);
    m_quoteInput->setPlaceholderText(QStringLiteral("1\n2\n3"));
    leftColumn->addWidget(m_quoteInput, 1);
    columns->addLayout(leftColumn, 1);

    auto *rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(6);
    rightColumn->addWidget(makeCaption(QStringLiteral("结果（只读）"), page));
    m_quoteOutput = makeEditor(page, true);
    // 结果通常是一整行很长的内容，按宽度折行才不用一直横向拖动。
    m_quoteOutput->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    rightColumn->addWidget(m_quoteOutput, 1);
    columns->addLayout(rightColumn, 1);

    layout->addLayout(columns, 1);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *pasteBtn = new QPushButton(QStringLiteral("粘贴"), page);
    auto *clearBtn = new QPushButton(QStringLiteral("清空"), page);
    auto *copyBtn = new QPushButton(QStringLiteral("复制结果"), page);
    copyBtn->setProperty("accent", true);
    actions->addWidget(pasteBtn);
    actions->addWidget(clearBtn);
    actions->addWidget(copyBtn);
    actions->addStretch(1);
    layout->addLayout(actions);

    m_quoteStatus = makeStatus(page);
    layout->addWidget(m_quoteStatus);

    connect(m_quoteInput, &QPlainTextEdit::textChanged, this, &TextKitPanel::updateQuote);
    connect(m_quoteMark, &QComboBox::currentIndexChanged, this, &TextKitPanel::updateQuote);
    connect(m_quoteSeparator, &QComboBox::currentIndexChanged, this, &TextKitPanel::updateQuote);
    connect(m_quoteTrim, &QCheckBox::toggled, this, &TextKitPanel::updateQuote);
    connect(m_quoteDropEmpty, &QCheckBox::toggled, this, &TextKitPanel::updateQuote);
    connect(m_quoteUnique, &QCheckBox::toggled, this, &TextKitPanel::updateQuote);

    connect(pasteBtn, &QPushButton::clicked, this, [this] {
        pasteInto(m_quoteInput, m_quoteStatus);
    });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_quoteInput->clear();
    });
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        copyToClipboard(m_quoteOutput->toPlainText(), QStringLiteral("拼接结果"), m_quoteStatus);
    });
    return page;
}

void TextKitPanel::updateQuote()
{
    textlines::QuoteOptions options;
    const QString quote = m_quoteMark->currentData().toString();
    if (!quote.isEmpty()) {
        options.leftQuote = quote.at(0);
        options.rightQuote = quote.at(0);
    }
    options.separator = m_quoteSeparator->currentData().toString();
    options.trimSpaces = m_quoteTrim->isChecked();
    options.dropEmpty = m_quoteDropEmpty->isChecked();
    options.unique = m_quoteUnique->isChecked();

    const QString input = m_quoteInput->toPlainText();
    const QStringList items = textlines::quoteItems(input, options);
    m_quoteOutput->setPlainText(items.join(options.separator));

    if (input.isEmpty()) {
        setStatusText(m_quoteStatus, QStringLiteral("在左侧粘贴内容后，这里实时显示拼接结果。"));
    } else if (items.isEmpty()) {
        setStatusText(m_quoteStatus,
                      QStringLiteral("输入里没有可用内容：全是空行（已勾选「去掉空行」时会被全部去掉）。"),
                      true);
    } else {
        const int rawLines = textlines::splitLines(input).size();
        setStatusText(m_quoteStatus,
                      QStringLiteral("共 %1 项（输入 %2 行），已拼成一行。").arg(items.size()).arg(rawLines));
    }
}

// ---------------------------------------------------------------- 对比页

QWidget *TextKitPanel::buildDiffTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeHint(
        QStringLiteral("左右各粘一段文字，差异实时标亮：左侧红色 = 只在左侧，右侧绿色 = 只在右侧，"
                       "深色字符块 = 同一行里被改动的字词。"),
        page));

    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);

    auto *leftColumn = new QVBoxLayout;
    leftColumn->setSpacing(6);
    m_leftCaption = makeCaption(QStringLiteral("文本 A"), page);
    leftColumn->addWidget(m_leftCaption);
    m_leftEdit = makeEditor(page, false);
    m_leftEdit->setPlaceholderText(QStringLiteral("粘贴第一段文字"));
    leftColumn->addWidget(m_leftEdit, 1);
    columns->addLayout(leftColumn, 1);

    auto *rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(6);
    m_rightCaption = makeCaption(QStringLiteral("文本 B"), page);
    rightColumn->addWidget(m_rightCaption);
    m_rightEdit = makeEditor(page, false);
    m_rightEdit->setPlaceholderText(QStringLiteral("粘贴第二段文字"));
    rightColumn->addWidget(m_rightEdit, 1);
    columns->addLayout(rightColumn, 1);

    layout->addLayout(columns, 1);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *pasteLeftBtn = new QPushButton(QStringLiteral("粘贴到左"), page);
    auto *pasteRightBtn = new QPushButton(QStringLiteral("粘贴到右"), page);
    auto *swapBtn = new QPushButton(QStringLiteral("交换左右"), page);
    auto *clearBtn = new QPushButton(QStringLiteral("清空"), page);
    auto *copyBtn = new QPushButton(QStringLiteral("复制差异清单"), page);
    copyBtn->setProperty("accent", true);
    actions->addWidget(pasteLeftBtn);
    actions->addWidget(pasteRightBtn);
    actions->addWidget(swapBtn);
    actions->addWidget(clearBtn);
    actions->addWidget(copyBtn);
    actions->addStretch(1);
    layout->addLayout(actions);

    m_diffStatus = makeStatus(page);
    layout->addWidget(m_diffStatus);

    connect(m_leftEdit, &QPlainTextEdit::textChanged, this, &TextKitPanel::scheduleDiff);
    connect(m_rightEdit, &QPlainTextEdit::textChanged, this, &TextKitPanel::scheduleDiff);
    connect(pasteLeftBtn, &QPushButton::clicked, this, [this] {
        pasteInto(m_leftEdit, m_diffStatus);
    });
    connect(pasteRightBtn, &QPushButton::clicked, this, [this] {
        pasteInto(m_rightEdit, m_diffStatus);
    });
    connect(swapBtn, &QPushButton::clicked, this, [this] {
        // 交换后立刻重算一次：等防抖会让界面在 180ms 内显示上一轮的旧高亮，
        // 看起来像「交换没生效」。立即算完再让定时器自然超时一次也无害。
        const QString left = m_leftEdit->toPlainText();
        const QString right = m_rightEdit->toPlainText();
        m_leftEdit->setPlainText(right);
        m_rightEdit->setPlainText(left);
        runDiff();
    });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_leftEdit->clear();
        m_rightEdit->clear();
        runDiff();
    });
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        copyToClipboard(m_lastDiffReport, QStringLiteral("差异清单"), m_diffStatus);
    });
    return page;
}

void TextKitPanel::scheduleDiff()
{
    m_diffTimer->start();   // 重复 start() 会重置计时，天然实现「停手才比较」
}

void TextKitPanel::runDiff()
{
    m_diffTimer->stop();

    const QString leftText = m_leftEdit->toPlainText();
    const QString rightText = m_rightEdit->toPlainText();

    const textdiff::Result result = textdiff::compare(leftText, rightText);
    applyDiffHighlight(m_leftEdit, result, true);
    applyDiffHighlight(m_rightEdit, result, false);

    m_leftCaption->setText(QStringLiteral("文本 A · %1 行").arg(result.leftLines));
    m_rightCaption->setText(QStringLiteral("文本 B · %1 行").arg(result.rightLines));
    m_lastDiffReport = buildDiffReport(result);

    if (leftText.isEmpty() && rightText.isEmpty()) {
        setStatusText(m_diffStatus, QStringLiteral("把两段文字分别粘到左右两侧，差异会实时标亮。"));
        return;
    }
    if (result.approximate) {
        // 规则 3 的同类要求：能力有折扣就明说，不要让用户以为标出来的就是全部差异。
        setStatusText(m_diffStatus,
                      QStringLiteral("文本过大（超过 %1 行或 %2 万格），已退化为按行号对齐的粗略比较："
                                     "中间插入一行会让后面每行都显示为不同。")
                          .arg(textdiff::detail::kMaxLines)
                          .arg(textdiff::detail::kMaxCells / 10000),
                      true);
        return;
    }
    if (result.differenceCount() == 0) {
        setStatusText(m_diffStatus,
                      QStringLiteral("两段文字完全一致（各 %1 行）。").arg(result.leftLines));
        return;
    }
    setStatusText(m_diffStatus,
                  QStringLiteral("共 %1 处不同：修改 %2 行 · 仅左 %3 行 · 仅右 %4 行"
                                 "（左 %5 行 / 右 %6 行，相同 %7 行）。")
                      .arg(result.differenceCount())
                      .arg(result.changedLines)
                      .arg(result.removedLines)
                      .arg(result.addedLines)
                      .arg(result.leftLines)
                      .arg(result.rightLines)
                      .arg(result.equalLines));
}

// ---------------------------------------------------------------- 差异提取页

QWidget *TextKitPanel::buildSetTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeHint(
        QStringLiteral("左右各粘一段（每行一项），下面分别列出各自独有的值："
                       "左边 1 3 4、右边 1 4 2 → 只在左 3、只在右 2。"),
        page));

    auto *options = new QHBoxLayout;
    options->setSpacing(8);
    m_setIgnoreEmpty = new QCheckBox(QStringLiteral("忽略空行"), page);
    m_setIgnoreEmpty->setChecked(true);
    m_setByCount = new QCheckBox(QStringLiteral("按出现次数比较"), page);
    m_setByCount->setToolTip(
        QStringLiteral("勾选后按「出现几次」算差额，适合两版日志比少了哪几条；"
                       "不勾选时同一个值只列一次（集合语义）。"));
    options->addWidget(m_setIgnoreEmpty);
    options->addWidget(m_setByCount);
    options->addSpacing(10);
    auto *pasteLeftBtn = new QPushButton(QStringLiteral("粘贴到左"), page);
    auto *pasteRightBtn = new QPushButton(QStringLiteral("粘贴到右"), page);
    auto *swapBtn = new QPushButton(QStringLiteral("交换左右"), page);
    auto *clearBtn = new QPushButton(QStringLiteral("清空"), page);
    options->addWidget(pasteLeftBtn);
    options->addWidget(pasteRightBtn);
    options->addWidget(swapBtn);
    options->addWidget(clearBtn);
    options->addStretch(1);
    layout->addLayout(options);

    // 每侧一列：上面是输入，下面是这一侧独有的值。列内上下叠放而不是把四个框排成
    // 两行，是为了让「只在左侧」紧跟文本 A —— 读的时候不用来回跳行找对应关系。
    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);

    auto *leftColumn = new QVBoxLayout;
    leftColumn->setSpacing(6);
    leftColumn->addWidget(makeCaption(QStringLiteral("文本 A（每行一项）"), page));
    m_setLeftEdit = makeEditor(page, false);
    m_setLeftEdit->setPlaceholderText(QStringLiteral("1\n3\n4"));
    leftColumn->addWidget(m_setLeftEdit, 3);
    m_leftCountLabel = makeCaption(QStringLiteral("只在左侧"), page);
    leftColumn->addWidget(m_leftCountLabel);
    m_onlyLeft = makeEditor(page, true);
    // 结果框染色（浅红/浅绿）与对比页的语义保持一致：红=左侧独有，绿=右侧独有。
    xtools::theme::applyStyle(m_onlyLeft, QStringLiteral(
        "QPlainTextEdit{background:@diff-removed-pane-bg;border:1px solid @diff-removed-pane-border;"
        "border-radius:8px;padding:6px 8px;color:@diff-removed-pane-text;}"));
    m_onlyLeft->setMinimumHeight(72);   // 结果通常只有几行，给个下限免得窗口变矮时被压没
    leftColumn->addWidget(m_onlyLeft, 2);
    columns->addLayout(leftColumn, 1);

    auto *rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(6);
    rightColumn->addWidget(makeCaption(QStringLiteral("文本 B（每行一项）"), page));
    m_setRightEdit = makeEditor(page, false);
    m_setRightEdit->setPlaceholderText(QStringLiteral("1\n4\n2"));
    rightColumn->addWidget(m_setRightEdit, 3);
    m_rightCountLabel = makeCaption(QStringLiteral("只在右侧"), page);
    rightColumn->addWidget(m_rightCountLabel);
    m_onlyRight = makeEditor(page, true);
    xtools::theme::applyStyle(m_onlyRight, QStringLiteral(
        "QPlainTextEdit{background:@diff-added-pane-bg;border:1px solid @diff-added-pane-border;"
        "border-radius:8px;padding:6px 8px;color:@diff-added-pane-text;}"));
    m_onlyRight->setMinimumHeight(72);
    rightColumn->addWidget(m_onlyRight, 2);
    columns->addLayout(rightColumn, 1);

    layout->addLayout(columns, 1);

    auto *copyRow = new QHBoxLayout;
    copyRow->setSpacing(8);
    m_copyLeftBtn = new QPushButton(QStringLiteral("复制只在左侧"), page);
    m_copyRightBtn = new QPushButton(QStringLiteral("复制只在右侧"), page);
    m_copyAllBtn = new QPushButton(QStringLiteral("复制全部差异"), page);
    m_copyAllBtn->setProperty("accent", true);
    copyRow->addWidget(m_copyLeftBtn);
    copyRow->addWidget(m_copyRightBtn);
    copyRow->addWidget(m_copyAllBtn);
    copyRow->addStretch(1);
    layout->addLayout(copyRow);

    m_setStatus = makeStatus(page);
    layout->addWidget(m_setStatus);

    connect(m_setLeftEdit, &QPlainTextEdit::textChanged, this, &TextKitPanel::updateSetDiff);
    connect(m_setRightEdit, &QPlainTextEdit::textChanged, this, &TextKitPanel::updateSetDiff);
    connect(m_setIgnoreEmpty, &QCheckBox::toggled, this, &TextKitPanel::updateSetDiff);
    connect(m_setByCount, &QCheckBox::toggled, this, &TextKitPanel::updateSetDiff);

    connect(pasteLeftBtn, &QPushButton::clicked, this, [this] {
        pasteInto(m_setLeftEdit, m_setStatus);
    });
    connect(pasteRightBtn, &QPushButton::clicked, this, [this] {
        pasteInto(m_setRightEdit, m_setStatus);
    });
    connect(swapBtn, &QPushButton::clicked, this, [this] {
        const QString left = m_setLeftEdit->toPlainText();
        m_setLeftEdit->setPlainText(m_setRightEdit->toPlainText());
        m_setRightEdit->setPlainText(left);
    });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_setLeftEdit->clear();   // 两个都清；textChanged 会各自触发重算
        m_setRightEdit->clear();
    });
    connect(m_copyLeftBtn, &QPushButton::clicked, this, [this] {
        copyToClipboard(m_onlyLeft->toPlainText(), QStringLiteral("只在左侧的值"), m_setStatus);
    });
    connect(m_copyRightBtn, &QPushButton::clicked, this, [this] {
        copyToClipboard(m_onlyRight->toPlainText(), QStringLiteral("只在右侧的值"), m_setStatus);
    });
    connect(m_copyAllBtn, &QPushButton::clicked, this, [this] {
        // 「全部差异」= 只在左 + 只在右，各自保持原文顺序，直接用换行拼起来。
        // 不在这里再加侧别前缀：多数场景是拿这份结果去搜/比对，多一行标题反而碍事。
        QStringList all = textlines::splitLines(m_onlyLeft->toPlainText());
        const QStringList right = textlines::splitLines(m_onlyRight->toPlainText());
        for (const QString &line : right)
            all.append(line);
        copyToClipboard(all.join(QLatin1Char('\n')), QStringLiteral("全部差异"), m_setStatus);
    });
    return page;
}

void TextKitPanel::updateSetDiff()
{
    const QString left = m_setLeftEdit->toPlainText();
    const QString right = m_setRightEdit->toPlainText();

    // 先统一清成空状态，再按各种分支重新填；这样任何一次重算都不会留下上一轮的行数。
    const auto resetEmpty = [this] {
        m_onlyLeft->clear();
        m_onlyRight->clear();
        m_leftCountLabel->setText(QStringLiteral("只在左侧"));
        m_rightCountLabel->setText(QStringLiteral("只在右侧"));
        m_copyLeftBtn->setEnabled(false);
        m_copyRightBtn->setEnabled(false);
        m_copyAllBtn->setEnabled(false);
    };

    if (left.trimmed().isEmpty() && right.trimmed().isEmpty()) {
        resetEmpty();
        setStatusText(m_setStatus, QStringLiteral("左右各粘一段（每行一项），这里会列出各自独有的值。"));
        return;
    }

    const textlines::SetDiff diff = textlines::compareSets(
        left, right, m_setIgnoreEmpty->isChecked(), m_setByCount->isChecked());

    m_onlyLeft->setPlainText(diff.onlyLeft.join(QLatin1Char('\n')));
    m_onlyRight->setPlainText(diff.onlyRight.join(QLatin1Char('\n')));
    m_leftCountLabel->setText(QStringLiteral("只在左侧 · %1 项").arg(diff.onlyLeft.size()));
    m_rightCountLabel->setText(QStringLiteral("只在右侧 · %1 项").arg(diff.onlyRight.size()));
    m_copyLeftBtn->setEnabled(!diff.onlyLeft.isEmpty());
    m_copyRightBtn->setEnabled(!diff.onlyRight.isEmpty());
    m_copyAllBtn->setEnabled(!diff.onlyLeft.isEmpty() || !diff.onlyRight.isEmpty());

    if (diff.onlyLeft.isEmpty() && diff.onlyRight.isEmpty()) {
        setStatusText(m_setStatus,
                      QStringLiteral("两段内容完全一致（左 %1 项 / 右 %2 项，共有 %3 项）。")
                          .arg(diff.leftItems)
                          .arg(diff.rightItems)
                          .arg(diff.common.size()));
        return;
    }
    setStatusText(m_setStatus,
                  QStringLiteral("左 %1 项 / 右 %2 项：只在左 %3 项，只在右 %4 项，共有 %5 项。")
                      .arg(diff.leftItems)
                      .arg(diff.rightItems)
                      .arg(diff.onlyLeft.size())
                      .arg(diff.onlyRight.size())
                      .arg(diff.common.size()));
}

// ---------------------------------------------------------------- 剪贴板

void TextKitPanel::pasteInto(QPlainTextEdit *edit, QLabel *status)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        setStatusText(status, QStringLiteral("剪贴板不可用。"), true);
        return;
    }
    const QString text = clipboard->text();
    if (text.isEmpty()) {
        setStatusText(status, QStringLiteral("剪贴板里没有文本内容。"), true);
        return;
    }
    edit->setPlainText(text);   // 替换而不是追加：连续粘贴两段会把内容混在一起
    setStatusText(status, QStringLiteral("已从剪贴板粘贴 %1 行。").arg(textlines::splitLines(text).size()));
}

void TextKitPanel::copyToClipboard(const QString &text, const QString &label, QLabel *status)
{
    if (text.isEmpty()) {
        setStatusText(status, QStringLiteral("没有可复制的内容。"), true);
        return;
    }
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        setStatusText(status, QStringLiteral("剪贴板不可用。"), true);
        return;
    }
    clipboard->setText(text);
    // QClipboard::setText() 没有返回值：Windows 上剪贴板被别的进程占住时，
    // Qt 只往 stderr 打一条警告，界面若照常报「已复制」就是静默失败。回读一次确认。
    if (clipboard->text() != text) {
        setStatusText(status, QStringLiteral("剪贴板写入失败（可能被其他程序占用），请重试。"), true);
        return;
    }
    setStatusText(status, QStringLiteral("已复制%1（%2 个字符）。").arg(label).arg(text.size()));
}

void TextKitPanel::setStatusText(QLabel *label, const QString &text, bool error)
{
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@%1;")
                                     .arg(xtools::theme::tokenName(error ? palette::error : palette::hint)));
    label->setText(text);
}

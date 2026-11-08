#include "RegexPanel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPair>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 输入防抖间隔：每敲一个字符都重新编译正则 + 重建表格会明显掉帧，
/// 停手 120ms 再算，交互上察觉不到延迟。
constexpr int kDebounceMs = 120;

/// 统计上限与时间预算，必须与状态行文案保持一致（改这里就一起改提示）。
constexpr int kMaxMatches = 2000;
constexpr int kTimeBudgetMs = 1500;

/// 表格单元格里最多显示多少个字符（长匹配只显示开头，完整内容在悬浮提示里，
/// 真正要全文用「复制全部匹配」）。
constexpr int kCellTextLimit = 160;

/// 配色，集中定义避免各处写字面量漂移（与全局 qss 同一套浅色系）。
namespace palette {
constexpr const char *hint = "#64748B";     // 常规提示
constexpr const char *error = "#DC2626";    // 编译错误
constexpr const char *ok = "#047857";       // 有匹配
constexpr const char *matchBg = "#FDE68A";  // 全部匹配的底色（浅黄，白底上要能一眼看到）
constexpr const char *currentBg = "#FBBF24"; // 当前定位那条的底色（深黄）
} // namespace palette

/// 小节标题，与 TextKitPanel / WebSocketTool 同一号灰蓝小字。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;color:#64748B;"));
    return label;
}

/// 副标题 / 统计信息：灰色小字，不抢主标题。
QLabel *makeHintLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("font-size:12px;color:%1;").arg(QLatin1String(palette::hint)));
    return label;
}

/// 代码类编辑框统一外观：等宽字体 + 白底描边圆角。
/// 插件面板不进全局 qss 的 objectName 规则，跟其它插件一样就地写内联样式。
void styleMonoEdit(QPlainTextEdit *edit)
{
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{background:#FFFFFF;border:1px solid #E2E8F0;border-radius:8px;"
        "padding:6px 8px;color:#0F172A;}"));
}

/// 结果表外观，与 filelock 的 #flTable 同一套观感（那边写在全局 qss 里，
/// 这里就地内联，免得为了一个插件去改全局样式）。
void styleTable(QTableWidget *table)
{
    table->setStyleSheet(QStringLiteral(
        "QTableWidget{background:#FFFFFF;border:1px solid #E8EDF5;border-radius:8px;"
        "outline:none;gridline-color:#F1F5F9;}"
        "QTableWidget::item{padding:0px 8px;color:#334155;}"
        "QTableWidget::item:selected{background:#EEF2FF;color:#1D4ED8;}"
        "QHeaderView::section{background:#FFFFFF;color:#64748B;font-size:12px;font-weight:600;"
        "border:none;border-bottom:1px solid #E2E8F0;padding:0px 8px;}"
        "QTableCornerButton::section{background:#FFFFFF;border:none;}"));
}

/// 带提示的自绘复选框，省掉每个都写三行 setToolTip。
QCheckBox *makeFlag(QWidget *parent, const QString &text, const QString &tip)
{
    auto *box = new QCheckBox(text, parent);
    box->setToolTip(tip);
    return box;
}

/// 表格里显示匹配文本：换行/制表符转成可见转义，否则单元格会被撑成多行；
/// 超长只留开头，避免一行几万字符把表格撑爆。
QString cellText(const QString &text, int limit = kCellTextLimit)
{
    QString flat = text;
    flat.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    flat.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    flat.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    if (flat.size() > limit)
        flat = flat.left(limit) + QStringLiteral("…");
    return flat;
}

/// 捕获组摘要：`1 xxx  2 yyy`。空组用 ∅ 表示（未参与匹配与匹配到空串无法区分，
/// 对使用者来说都是「这一组没内容」）。
QString groupsText(const QStringList &groups)
{
    if (groups.isEmpty())
        return QStringLiteral("—");
    QStringList parts;
    for (int i = 0; i < groups.size(); ++i) {
        const QString value = groups.at(i).isEmpty() ? QStringLiteral("∅") : cellText(groups.at(i), 60);
        parts << QStringLiteral("%1 %2").arg(i + 1).arg(value);
    }
    return parts.join(QStringLiteral("  "));
}

/// 「常用正则」菜单项。都是离线可用的通用写法，填进去还能继续改。
QVector<QPair<QString, QString>> presets()
{
    return {
        {QStringLiteral("网址 URL"), QStringLiteral("https?://[\\w\\-._~:/?#\\[\\]@!$&'()*+,;=%]+")},
        {QStringLiteral("邮箱"), QStringLiteral("[\\w.%+\\-]+@[A-Za-z0-9.\\-]+\\.[A-Za-z]{2,}")},
        {QStringLiteral("手机号（大陆）"), QStringLiteral("(?<!\\d)1[3-9]\\d{9}(?!\\d)")},
        {QStringLiteral("IPv4"), QStringLiteral("\\b(?:(?:25[0-5]|2[0-4]\\d|1\\d{2}|[1-9]?\\d)\\.){3}"
                                               "(?:25[0-5]|2[0-4]\\d|1\\d{2}|[1-9]?\\d)\\b")},
        {QStringLiteral("中文字符"), QStringLiteral("[\\x{4e00}-\\x{9fa5}]+")},
        {QStringLiteral("日期（YYYY-MM-DD）"), QStringLiteral("\\b(\\d{4})-(\\d{2})-(\\d{2})\\b")},
        {QStringLiteral("HTML 标签"), QStringLiteral("</?[A-Za-z][^>]*>")},
    };
}

} // namespace

///
/// 构造面板：正则输入 → 标志 → 文本/结果的竖向分栏 → 状态行。
///
RegexPanel::RegexPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐；上方由宿主补留白
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(8);

    // ---- 1. 正则输入行 ---------------------------------------------------
    // 页头已经写了工具名，这里不再重复标题；语法方言的提示放到标志行最右侧，
    // 既不占竖向空间，又能在用户纳闷「为什么 JS 的正则报错」时被看到。
    auto *patternRow = new QHBoxLayout;
    patternRow->setSpacing(8);

    m_pattern = new QLineEdit(this);
    m_pattern->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_pattern->setClearButtonEnabled(true);
    m_pattern->setPlaceholderText(QStringLiteral("输入正则，例如 \\b\\d{4}-\\d{2}-\\d{2}\\b"));
    m_pattern->setToolTip(QStringLiteral(
        "Qt 使用 PCRE2 语法：JS 的 \\u4e00 要写成 \\x{4e00}；"
        "Python 的 (?P<name>...) 与 (?<name>...) 都支持"));
    patternRow->addWidget(m_pattern, 1);

    auto *presetButton = new QPushButton(QStringLiteral("常用正则"), this);
    presetButton->setToolTip(QStringLiteral("挑一个常用表达式填进输入框，填进去后还能继续改"));
    auto *presetMenu = new QMenu(presetButton);
    const auto items = presets();
    for (const auto &item : items) {
        QAction *action = presetMenu->addAction(item.first);
        const QString pattern = item.second;
        connect(action, &QAction::triggered, this, [this, pattern] {
            m_pattern->setText(pattern);
            m_pattern->setFocus();
        });
    }
    presetButton->setMenu(presetMenu);
    patternRow->addWidget(presetButton);
    layout->addLayout(patternRow);

    // ---- 2. 标志行 -------------------------------------------------------
    auto *flagRow = new QHBoxLayout;
    flagRow->setSpacing(14);
    m_case = makeFlag(this, QStringLiteral("忽略大小写 (i)"),
                      QStringLiteral("PCRE2 的 CASELESS：A 与 a 视为相同"));
    m_multiline = makeFlag(this, QStringLiteral("多行 (m)"),
                           QStringLiteral("^ 与 $ 匹配每一行的首尾，而不是整段文本的首尾"));
    m_dotAll = makeFlag(this, QStringLiteral("点匹配换行 (s)"),
                        QStringLiteral("DOTALL：. 也匹配换行符（默认不匹配）"));
    m_extended = makeFlag(this, QStringLiteral("忽略空白 (x)"),
                          QStringLiteral("EXTENDED：忽略未转义的空白与 # 注释，方便把长正则拆成多行写"));
    m_inverted = makeFlag(this, QStringLiteral("贪婪反转 (U)"),
                          QStringLiteral("INVERTED_GREEDY：* + ? 默认改成非贪婪，想贪婪要额外写 ?"));
    for (QCheckBox *box : {m_case, m_multiline, m_dotAll, m_extended, m_inverted})
        flagRow->addWidget(box);
    flagRow->addStretch(1);
    flagRow->addWidget(makeHintLabel(QStringLiteral("PCRE2 语法（Qt）"), this));
    layout->addLayout(flagRow);

    // ---- 3. 文本 + 结果（竖向 splitter，比例可自己拖）--------------------
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildSubjectSide());
    splitter->addWidget(buildResultSide());
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({340, 220});
    layout->addWidget(splitter, 1);

    // ---- 4. 状态行 -------------------------------------------------------
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    // ---- 信号 ------------------------------------------------------------
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(kDebounceMs);
    connect(m_timer, &QTimer::timeout, this, &RegexPanel::runMatch);

    connect(m_pattern, &QLineEdit::textChanged, this, &RegexPanel::scheduleMatch);
    connect(m_subject, &QPlainTextEdit::textChanged, this, &RegexPanel::scheduleMatch);
    // 行数 / 字符数跟着文本变（与匹配解耦：改一个字就刷新，不必等防抖结束）
    connect(m_subject, &QPlainTextEdit::textChanged, this, &RegexPanel::updateSubjectInfo);
    for (QCheckBox *box : {m_case, m_multiline, m_dotAll, m_extended, m_inverted})
        connect(box, &QCheckBox::toggled, this, &RegexPanel::scheduleMatch);

    updateSubjectInfo();
    m_copyAll->setEnabled(false);
    setStatus(QStringLiteral("输入正则后自动匹配 · Qt 用 PCRE2 语法，JS 的 \\uXXXX 需写成 \\x{XXXX}"),
              StatusKind::Hint);
}

QWidget *RegexPanel::buildSubjectSide()
{
    auto *side = new QWidget(this);
    auto *box = new QVBoxLayout(side);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(makeCaption(QStringLiteral("测试文本"), side));
    m_subjectInfo = makeHintLabel(QString(), side);
    row->addWidget(m_subjectInfo);
    row->addStretch(1);

    auto *paste = new QPushButton(QStringLiteral("粘贴"), side);
    paste->setToolTip(QStringLiteral("用剪贴板里的文本替换当前内容"));
    connect(paste, &QPushButton::clicked, this, &RegexPanel::pasteSubject);
    row->addWidget(paste);

    auto *clear = new QPushButton(QStringLiteral("清空"), side);
    connect(clear, &QPushButton::clicked, this, &RegexPanel::clearSubject);
    row->addWidget(clear);
    box->addLayout(row);

    m_subject = new QPlainTextEdit(side);
    styleMonoEdit(m_subject);
    m_subject->setPlaceholderText(QStringLiteral(
        "粘贴要匹配的文本（日志、代码、HTML 都行），输入时实时匹配并高亮"));
    box->addWidget(m_subject, 1);
    return side;
}

QWidget *RegexPanel::buildResultSide()
{
    auto *side = new QWidget(this);
    auto *box = new QVBoxLayout(side);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(makeCaption(QStringLiteral("匹配结果"), side));
    row->addWidget(makeHintLabel(QStringLiteral("点一行可在文本里定位"), side));
    row->addStretch(1);

    m_copyAll = new QPushButton(QStringLiteral("复制全部匹配"), side);
    m_copyAll->setToolTip(QStringLiteral("每条匹配占一行写入剪贴板"));
    connect(m_copyAll, &QPushButton::clicked, this, &RegexPanel::copyAllMatches);
    row->addWidget(m_copyAll);
    box->addLayout(row);

    m_table = new QTableWidget(side);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(QStringList{
        QStringLiteral("序号"), QStringLiteral("行"), QStringLiteral("位置"),
        QStringLiteral("匹配文本"), QStringLiteral("捕获组")});
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(24);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    QHeaderView *header = m_table->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionsClickable(false);
    header->setSectionResizeMode(0, QHeaderView::Fixed);
    header->setSectionResizeMode(1, QHeaderView::Fixed);
    header->setSectionResizeMode(2, QHeaderView::Fixed);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    header->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 52);
    m_table->setColumnWidth(1, 52);
    m_table->setColumnWidth(2, 96);

    styleTable(m_table);
    box->addWidget(m_table, 1);

    // 鼠标点选与键盘上下移动都要能定位；currentCellChanged 在填表时也会触发，
    // 用 m_fillingTable 挡掉，否则每填一行就会把光标抢到那条匹配上。
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) { locateMatch(row); });
    connect(m_table, &QTableWidget::currentCellChanged, this,
            [this](int currentRow, int, int, int) {
                if (m_fillingTable || currentRow < 0)
                    return;
                locateMatch(currentRow);
            });

    return side;
}

void RegexPanel::scheduleMatch()
{
    m_timer->start();
}

void RegexPanel::runMatch()
{
    regexmatch::Options options;
    options.caseInsensitive = m_case->isChecked();
    options.multiline = m_multiline->isChecked();
    options.dotAll = m_dotAll->isChecked();
    options.extended = m_extended->isChecked();
    options.invertedGreedy = m_inverted->isChecked();
    options.maxMatches = kMaxMatches;
    options.timeBudgetMs = kTimeBudgetMs;

    applyResult(regexmatch::run(m_pattern->text(), m_subject->toPlainText(), options));
}

void RegexPanel::applyResult(const regexmatch::Result &result)
{
    m_matches = result.matches;
    m_currentRow = -1;

    if (result.emptyPattern) {
        fillTable();
        clearHighlight();
        setStatus(QStringLiteral("输入正则后自动匹配 · Qt 用 PCRE2 语法，JS 的 \\uXXXX 需写成 \\x{XXXX}"),
                  StatusKind::Hint);
        return;
    }

    if (!result.ok) {
        fillTable();
        clearHighlight();
        QString text = QStringLiteral("正则不合法：%1").arg(result.error);
        if (result.errorOffset > 0)
            text += QStringLiteral("（第 %1 个字符附近）").arg(result.errorOffset + 1);
        // 从 JS 抄过来的正则十有八九是 \uXXXX：先替用户把原因说出来，省得以为是工具坏了
        if (m_pattern->text().contains(QLatin1String("\\u")))
            text += QStringLiteral("；Qt 用的是 PCRE2 语法，JS 的 \\uXXXX 需改写成 \\x{XXXX}");
        setStatus(text, StatusKind::Error);
        return;
    }

    fillTable();
    highlightMatches();

    if (m_matches.isEmpty()) {
        setStatus(QStringLiteral("没有匹配 · %1 ms").arg(QString::number(result.elapsedMs, 'f', 1)),
                  StatusKind::Hint);
        return;
    }

    QString text = QStringLiteral("%1 个匹配").arg(m_matches.size());
    if (result.truncated)
        text += QStringLiteral("（已达 %1 条上限，后面的没统计）").arg(kMaxMatches);
    if (result.timedOut)
        text += QStringLiteral("（超过 %1 ms 预算，已中断）").arg(kTimeBudgetMs);
    text += QStringLiteral(" · %1 ms").arg(QString::number(result.elapsedMs, 'f', 1));
    setStatus(text, StatusKind::Ok);
}

void RegexPanel::fillTable()
{
    m_fillingTable = true;
    m_table->setRowCount(0);
    m_table->setRowCount(m_matches.size());

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (int row = 0; row < m_matches.size(); ++row) {
        const regexmatch::MatchItem &match = m_matches.at(row);

        // 位置写成「起–止」闭区间（与 regex101 的 Full match 0-9 一致）；
        // 零宽匹配没有结束位置，只写起点。
        const QString position = match.length > 0
            ? QStringLiteral("%1–%2").arg(match.start).arg(match.start + match.length - 1)
            : QStringLiteral("%1").arg(match.start);

        const QStringList cells{
            QString::number(row + 1),
            QString::number(match.line),
            position,
            match.length > 0 ? cellText(match.text) : QStringLiteral("（零宽匹配）"),
            groupsText(match.groups),
        };

        for (int col = 0; col < cells.size(); ++col) {
            auto *item = new QTableWidgetItem(cells.at(col));
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            if (col >= 3) {
                item->setFont(mono);
                // 单元格只显示前 160 字符，完整内容靠悬浮提示
                const QString full = col == 3 ? match.text : match.groups.join(QStringLiteral("  "));
                if (!full.isEmpty())
                    item->setToolTip(full.left(4000));
            }
            m_table->setItem(row, col, item);
        }
    }

    m_fillingTable = false;
    m_copyAll->setEnabled(!m_matches.isEmpty());
}

void RegexPanel::highlightMatches()
{
    QList<QTextEdit::ExtraSelection> selections;
    QTextDocument *document = m_subject->document();
    for (int i = 0; i < m_matches.size(); ++i) {
        const regexmatch::MatchItem &match = m_matches.at(i);
        // 零宽匹配没有字符可涂；长度非法（越界）也跳过，不让 setPosition 抛断言
        if (match.length <= 0 || match.start < 0 || match.start + match.length > document->characterCount())
            continue;

        QTextCursor cursor(document);
        cursor.setPosition(match.start);
        cursor.setPosition(match.start + match.length, QTextCursor::KeepAnchor);

        QTextEdit::ExtraSelection selection;
        selection.cursor = cursor;
        selection.format.setBackground(
            QColor(QLatin1String(i == m_currentRow ? palette::currentBg : palette::matchBg)));
        selections.append(selection);
    }
    m_subject->setExtraSelections(selections);
}

void RegexPanel::locateMatch(int row)
{
    if (row < 0 || row >= m_matches.size() || row == m_currentRow)
        return;

    const regexmatch::MatchItem &match = m_matches.at(row);
    QTextCursor cursor(m_subject->document());
    cursor.setPosition(match.start);
    if (match.length > 0)
        cursor.setPosition(match.start + match.length, QTextCursor::KeepAnchor);
    m_subject->setTextCursor(cursor);
    m_subject->centerCursor();

    m_currentRow = row;
    highlightMatches();
}

void RegexPanel::clearHighlight()
{
    m_subject->setExtraSelections({});
}

void RegexPanel::copyAllMatches()
{
    if (m_matches.isEmpty()) {
        setStatus(QStringLiteral("没有匹配可复制"), StatusKind::Hint);
        return;
    }

    QStringList lines;
    lines.reserve(m_matches.size());
    for (const regexmatch::MatchItem &match : m_matches)
        lines << match.text;
    const QString payload = lines.join(QLatin1Char('\n'));

    QGuiApplication::clipboard()->setText(payload);
    // setText 没有返回值：剪贴板被别的进程占住时只会静默失败，回读校验一次
    if (QGuiApplication::clipboard()->text() != payload) {
        setStatus(QStringLiteral("复制失败：剪贴板被其它程序占用"), StatusKind::Error);
        return;
    }
    setStatus(QStringLiteral("已复制 %1 条匹配").arg(m_matches.size()), StatusKind::Ok);
}

void RegexPanel::pasteSubject()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) {
        setStatus(QStringLiteral("剪贴板为空或不是文本"), StatusKind::Error);
        return;
    }
    m_subject->setPlainText(text);
    setStatus(QStringLiteral("已粘贴 %1 个字符").arg(text.size()), StatusKind::Hint);
}

void RegexPanel::clearSubject()
{
    m_subject->clear();
}

void RegexPanel::updateSubjectInfo()
{
    const QString text = m_subject->toPlainText();
    m_subjectInfo->setText(text.isEmpty()
        ? QStringLiteral("空")
        : QStringLiteral("%1 行 · %2 字符").arg(text.count(QLatin1Char('\n')) + 1).arg(text.size()));
}

void RegexPanel::setStatus(const QString &text, StatusKind kind)
{
    const char *color = palette::hint;
    if (kind == StatusKind::Error)
        color = palette::error;
    else if (kind == StatusKind::Ok)
        color = palette::ok;
    m_status->setStyleSheet(QStringLiteral("font-size:12px;color:%1;").arg(QLatin1String(color)));
    m_status->setText(text);
}

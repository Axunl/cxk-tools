#include "SqlPanel.h"

#include "SqlText.h"        // 词法 / 格式化 / 压缩 / MyBatis 还原（纯逻辑）
#include "ThemePalette.h"
#include "LayoutMetrics.h"    // 颜色令牌（宿主与插件共用）

#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 状态行配色集中在一处，避免各页签写字面量色值（换肤时令牌名不变、色值会变）。
constexpr auto kStatusHint = xtools::theme::Color::TextFaint;
constexpr auto kStatusText = xtools::theme::Color::TextBody;
constexpr auto kStatusWarn = xtools::theme::Color::DangerText;

/// 结果统计里"行数 / 字符数"的写法统一，免得三个页签各说各的。
QString describeSize(const QString &text)
{
    if (text.isEmpty())
        return QStringLiteral("空");
    const int lines = text.count(QLatin1Char('\n')) + 1;
    return QStringLiteral("%1 行 · %2 字符").arg(lines).arg(text.size());
}

/// 文本框统一外观：等宽字体 + 不自动换行（SQL 一行可能很长，换行会把结构绞碎）。
QPlainTextEdit *makeEditor(QWidget *parent, const QString &placeholder, bool readOnly)
{
    auto *edit = new QPlainTextEdit(parent);
    edit->setPlaceholderText(placeholder);
    edit->setReadOnly(readOnly);
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    if (readOnly) {
        // 只读结果框用下沉底色：一眼能看出"这块不用也不能改"
        xtools::theme::applyStyle(edit, QStringLiteral("QPlainTextEdit{background:@bg-alt;}"));
    }
    return edit;
}

/// 状态行统一外观：12px、允许换行（长警告不能把工具条撑宽）。
QLabel *makeStatusLabel(QWidget *parent, const QString &initial)
{
    auto *label = new QLabel(initial, parent);
    label->setWordWrap(true);
    xtools::theme::applyStyle(
        label, QStringLiteral("font-size:12px;color:@%1;")
                   .arg(xtools::theme::tokenName(kStatusHint)));
    return label;
}

} // namespace

SqlPanel::SqlPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐，视觉上像同一栏
    layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    layout->setSpacing(10);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    layout->addWidget(m_tabs, 1);

    m_tabs->addTab(buildMyBatisTab(), QStringLiteral("MyBatis 还原"));
    m_tabs->addTab(buildFormatTab(), QStringLiteral("格式化"));
    m_tabs->addTab(buildCompressTab(), QStringLiteral("压缩"));
}

// ===========================================================================
// 通用部件
// ===========================================================================

void SqlPanel::addResultButtons(QHBoxLayout *row, QWidget *page, QPlainTextEdit *input,
                               QPlainTextEdit *output, QLabel *status)
{
    auto *copy = new QPushButton(QStringLiteral("复制结果"), page);
    auto *clear = new QPushButton(QStringLiteral("清空"), page);

    row->addWidget(copy);
    row->addWidget(clear);
    row->addStretch(1);
    row->addWidget(status);

    connect(copy, &QPushButton::clicked, this, [this, copy, output, status] {
        const QString text = output->toPlainText();
        if (text.isEmpty()) {
            setStatus(status, QStringLiteral("还没有结果，先点左边的按钮"), true);
            return;
        }
        QGuiApplication::clipboard()->setText(text);
        copy->setText(QStringLiteral("已复制"));
        // singleShot 带接收者（按钮）：面板提前销毁时定时器随之作废，不会砸野指针
        QTimer::singleShot(1200, copy, [copy] { copy->setText(QStringLiteral("复制结果")); });
    });

    connect(clear, &QPushButton::clicked, this, [input, output, status] {
        input->clear();
        output->clear();
        status->clear();
        input->setFocus();
    });
}

void SqlPanel::addPanes(QWidget *page, QVBoxLayout *layout, QPlainTextEdit **inputOut,
                        QPlainTextEdit **outputOut, const QString &inputHint,
                        const QString &outputHint)
{
    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);

    *inputOut = makeEditor(page, inputHint, false);
    *outputOut = makeEditor(page, outputHint, true);

    splitter->addWidget(*inputOut);
    splitter->addWidget(*outputOut);
    // 上下各占一半：输入是"我要处理的东西"，输出是"处理结果"，没有主次
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 260});

    layout->addWidget(splitter, 1);
}

void SqlPanel::setStatus(QLabel *label, const QString &text, bool error)
{
    if (!label)
        return;
    const auto token = error ? kStatusWarn : kStatusText;
    xtools::theme::applyStyle(
        label, QStringLiteral("font-size:12px;color:@%1;").arg(xtools::theme::tokenName(token)));
    label->setText(text);
}

// ===========================================================================
// 页签一：MyBatis 日志还原
// ===========================================================================

QWidget *SqlPanel::buildMyBatisTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto *run = new QPushButton(QStringLiteral("还原 SQL"), page);
    run->setProperty("accent", true);   // 本页的主操作
    run->setToolTip(QStringLiteral("把 Parameters 行的值填回 SQL 的 ? 里（Ctrl+Enter）"));

    m_logFormat = new QCheckBox(QStringLiteral("格式化结果"), page);
    m_logFormat->setChecked(true);
    m_logFormat->setToolTip(QStringLiteral("还原后按子句换行缩进；关掉则保持日志里的原始单行"));
    m_logUpper = new QCheckBox(QStringLiteral("关键字大写"), page);
    m_logUpper->setChecked(true);
    m_logUpper->setToolTip(QStringLiteral("仅在上面的「格式化结果」勾选时生效"));

    m_logStatus = makeStatusLabel(page, QStringLiteral("粘日志里 Preparing / Parameters 两行"));

    addPanes(page, layout, &m_logInput, &m_logOutput,
             QStringLiteral("把 MyBatis 打的这两行日志粘进来（可以一次粘多条）：\n"
                            "==>  Preparing: select ... where id = ?\n"
                            "==> Parameters: 42(Long)"),
             QStringLiteral("还原结果会出现在这里"));

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    toolbar->addWidget(run);
    toolbar->addWidget(m_logFormat);
    toolbar->addWidget(m_logUpper);
    addResultButtons(toolbar, page, m_logInput, m_logOutput, m_logStatus);
    layout->addLayout(toolbar);

    connect(run, &QPushButton::clicked, this, &SqlPanel::runMyBatis);
    connect(m_logFormat, &QCheckBox::toggled, this, [this] {
        if (!m_logInput->toPlainText().trimmed().isEmpty())
            runMyBatis();
    });
    connect(m_logUpper, &QCheckBox::toggled, this, [this] {
        if (m_logFormat->isChecked() && !m_logInput->toPlainText().trimmed().isEmpty())
            runMyBatis();
    });

    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), page);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &SqlPanel::runMyBatis);
    return page;
}

void SqlPanel::runMyBatis()
{
    const QString log = m_logInput->toPlainText();
    if (log.trimmed().isEmpty()) {
        m_logOutput->clear();
        setStatus(m_logStatus, QStringLiteral("粘日志里 Preparing / Parameters 两行"));
        return;
    }

    const sqlfmt::MyBatisResult result = sqlfmt::restoreMyBatisLog(log);
    if (result.sql.isEmpty()) {
        m_logOutput->clear();
        setStatus(m_logStatus,
                  result.warnings.isEmpty() ? QStringLiteral("没解析出 SQL") : result.warnings.join(QStringLiteral("；")),
                  true);
        return;
    }

    QString text = result.sql;
    if (m_logFormat->isChecked()) {
        sqlfmt::FormatOptions options;
        options.upperKeywords = m_logUpper->isChecked();
        text = sqlfmt::formatSql(text, options);
    }
    m_logOutput->setPlainText(text);

    QString summary = QStringLiteral("还原 %1 条语句 · %2/%3 个占位符 · %4")
                          .arg(result.statements)
                          .arg(result.replaced)
                          .arg(result.placeholders)
                          .arg(describeSize(text));
    if (!result.warnings.isEmpty()) {
        setStatus(m_logStatus, summary + QStringLiteral(" · ") + result.warnings.join(QStringLiteral("；")), true);
        return;
    }
    setStatus(m_logStatus, summary);
}

// ===========================================================================
// 页签二：SQL 格式化
// ===========================================================================

QWidget *SqlPanel::buildFormatTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto *run = new QPushButton(QStringLiteral("格式化"), page);
    run->setProperty("accent", true);
    run->setToolTip(QStringLiteral("按子句换行、缩进、统一关键字大小写（Ctrl+Enter）"));

    m_fmtUpper = new QCheckBox(QStringLiteral("关键字大写"), page);
    m_fmtUpper->setChecked(true);
    m_fmtOneColumn = new QCheckBox(QStringLiteral("每列一行"), page);
    m_fmtOneColumn->setToolTip(QStringLiteral("SELECT 的每一列单独成行；列少时关掉更紧凑"));

    m_fmtStatus = makeStatusLabel(page, QStringLiteral("粘贴 SQL 后点「格式化」"));

    addPanes(page, layout, &m_fmtInput, &m_fmtOutput,
             QStringLiteral("粘贴要格式化的 SQL；可以一次粘多条语句（用 ; 分隔）"),
             QStringLiteral("格式化结果会出现在这里"));

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    toolbar->addWidget(run);
    toolbar->addWidget(m_fmtUpper);
    toolbar->addWidget(m_fmtOneColumn);
    addResultButtons(toolbar, page, m_fmtInput, m_fmtOutput, m_fmtStatus);
    layout->addLayout(toolbar);

    connect(run, &QPushButton::clicked, this, &SqlPanel::runFormat);
    connect(m_fmtUpper, &QCheckBox::toggled, this, [this] {
        if (!m_fmtInput->toPlainText().trimmed().isEmpty())
            runFormat();
    });
    connect(m_fmtOneColumn, &QCheckBox::toggled, this, [this] {
        if (!m_fmtInput->toPlainText().trimmed().isEmpty())
            runFormat();
    });

    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), page);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &SqlPanel::runFormat);
    return page;
}

void SqlPanel::runFormat()
{
    const QString sql = m_fmtInput->toPlainText();
    if (sql.trimmed().isEmpty()) {
        m_fmtOutput->clear();
        setStatus(m_fmtStatus, QStringLiteral("粘贴 SQL 后点「格式化」"));
        return;
    }

    sqlfmt::FormatOptions options;
    options.upperKeywords = m_fmtUpper->isChecked();
    options.oneColumnPerLine = m_fmtOneColumn->isChecked();

    const QString text = sqlfmt::formatSql(sql, options);
    m_fmtOutput->setPlainText(text);
    if (text.isEmpty()) {
        setStatus(m_fmtStatus, QStringLiteral("输入里没有可识别的 SQL 内容"), true);
        return;
    }
    setStatus(m_fmtStatus, QStringLiteral("已格式化 · ") + describeSize(text));
}

// ===========================================================================
// 页签三：SQL 压缩
// ===========================================================================

QWidget *SqlPanel::buildCompressTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(8);

    auto *run = new QPushButton(QStringLiteral("压缩"), page);
    run->setProperty("accent", true);
    run->setToolTip(QStringLiteral("去掉注释与多余空白，压成一行（Ctrl+Enter）"));

    m_minStatus = makeStatusLabel(page, QStringLiteral("粘贴 SQL 后点「压缩」"));

    addPanes(page, layout, &m_minInput, &m_minOutput,
             QStringLiteral("粘贴要压缩的 SQL（多行、带注释、带缩进都行）"),
             QStringLiteral("压缩结果会出现在这里（一行）"));

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    toolbar->addWidget(run);
    addResultButtons(toolbar, page, m_minInput, m_minOutput, m_minStatus);
    layout->addLayout(toolbar);

    connect(run, &QPushButton::clicked, this, &SqlPanel::runCompress);

    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), page);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, &SqlPanel::runCompress);
    return page;
}

void SqlPanel::runCompress()
{
    const QString sql = m_minInput->toPlainText();
    if (sql.trimmed().isEmpty()) {
        m_minOutput->clear();
        setStatus(m_minStatus, QStringLiteral("粘贴 SQL 后点「压缩」"));
        return;
    }

    const QString text = sqlfmt::compressSql(sql);
    m_minOutput->setPlainText(text);
    if (text.isEmpty()) {
        setStatus(m_minStatus, QStringLiteral("输入里没有可识别的 SQL 内容"), true);
        return;
    }
    setStatus(m_minStatus, QStringLiteral("已压缩 · %1（原 %2）").arg(describeSize(text), describeSize(sql)));
}

// ===========================================================================
// 首页带内容进来
// ===========================================================================

void SqlPanel::setInputText(const QString &text)
{
    // 一眼能认出是 MyBatis 日志就往第一个页签送，否则当普通 SQL 处理
    const bool looksLikeLog = text.contains(QLatin1String("Preparing:"))
        || text.contains(QLatin1String("Parameters:"));

    if (looksLikeLog) {
        m_tabs->setCurrentIndex(0);
        m_logInput->setPlainText(text);
        runMyBatis();
        return;
    }
    m_tabs->setCurrentIndex(1);
    m_fmtInput->setPlainText(text);
    runFormat();
}

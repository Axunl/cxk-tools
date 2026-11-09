#include "JsonFmtPanel.h"

#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用）

#include "JsonEditor.h"
#include "JsonText.h"   // 行列换算 / 转义等纯逻辑（可单独测试）

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 把光标从旧文本映射到格式化后的新文本（返回值已按新文本长度夹住）。
///
/// 分三种情况，按可靠性从高到低：
///  1. 光标在文末（后面只剩空白）→ 落到新文末。粘贴就是这个场景，也最常见；
///  2. 光标前的非空白字符序列在新文本里能原样找到 → 用那个位置。格式化只改空白，
///     只要键的顺序没变，这个映射就是**精确**的；
///  3. 对不上就退回"同一个行号 + 列号夹住"。为什么会对不上：QJsonDocument 输出时
///     按键名排序，顺序一变字符流就不再一一对应，锚点会指到别的 token 上；
///     保行号至少不会把人甩到文首。
int mapCursorPosition(const QString &oldText, int oldPos, const QString &newText)
{
    const int pos = qBound(0, oldPos, static_cast<int>(oldText.size()));

    bool atEnd = true;
    for (int i = pos; i < oldText.size(); ++i) {
        if (!oldText.at(i).isSpace()) {
            atEnd = false;
            break;
        }
    }
    if (atEnd) {
        int end = newText.size();
        while (end > 0 && newText.at(end - 1).isSpace())
            --end;
        return end;
    }
    if (pos == 0)
        return 0;

    QString oldPrefix;
    for (const QChar ch : oldText.left(pos)) {
        if (!ch.isSpace())
            oldPrefix += ch;
    }

    int anchor = -1;
    int seen = 0;
    for (int i = 0; i < newText.size(); ++i) {
        if (newText.at(i).isSpace())
            continue;
        if (++seen == oldPrefix.size()) {
            anchor = i + 1;
            break;
        }
    }
    if (anchor >= 0) {
        QString newPrefix;
        for (const QChar ch : newText.left(anchor)) {
            if (!ch.isSpace())
                newPrefix += ch;
        }
        if (newPrefix == oldPrefix)
            return anchor;   // 校验通过：确实是同一处
    }

    int line = 0;
    for (int i = 0; i < pos; ++i) {
        if (oldText.at(i) == QLatin1Char('\n'))
            ++line;
    }
    const int oldLineStart = oldText.lastIndexOf(QLatin1Char('\n'), pos - 1) + 1;
    const int column = pos - oldLineStart;

    const QStringList newLines = newText.split(QLatin1Char('\n'));
    const int target = qBound(0, line, newLines.size() - 1);
    int offset = 0;
    for (int i = 0; i < target; ++i)
        offset += newLines.at(i).size() + 1;
    return offset + qBound(0, column, newLines.at(target).size());
}

} // namespace

JsonFmtPanel::JsonFmtPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(10);

    // ---- 工具栏
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    auto *formatBtn = new QPushButton(QStringLiteral("格式化"), this);
    formatBtn->setProperty("accent", true);   // 主操作，用强调色
    auto *compactBtn = new QPushButton(QStringLiteral("压缩"), this);
    auto *escapeBtn = new QPushButton(QStringLiteral("转义"), this);
    auto *unescapeBtn = new QPushButton(QStringLiteral("反转义"), this);
    auto *copyBtn = new QPushButton(QStringLiteral("复制结果"), this);
    auto *clearBtn = new QPushButton(QStringLiteral("清空"), this);

    m_status = new QLabel(this);
    xtools::theme::applyStyle(
        m_status, QStringLiteral("font-size:12px;color:@%1;")
                      .arg(xtools::theme::tokenName(xtools::theme::Color::TextFaint)));

    toolbar->addWidget(formatBtn);
    toolbar->addWidget(compactBtn);
    toolbar->addWidget(escapeBtn);
    toolbar->addWidget(unescapeBtn);
    toolbar->addWidget(copyBtn);
    toolbar->addWidget(clearBtn);
    toolbar->addStretch(1);
    toolbar->addWidget(m_status);
    layout->addLayout(toolbar);

    // ---- 左右分栏
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    m_input = new JsonEditor(splitter);
    // 补全里把快捷键也写出来：这个能力没有按钮入口，只在提示里露出
    m_input->setPlaceholderText(
        QStringLiteral("粘贴 JSON 即自动格式化，Ctrl+空格 补全键名"));

    m_output = new JsonEditor(splitter);
    m_output->setReadOnly(true);
    m_output->setCompletionEnabled(false);
    m_output->setPlaceholderText(QStringLiteral("结果会出现在这里"));

    splitter->addWidget(m_input);
    splitter->addWidget(m_output);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({500, 500});
    layout->addWidget(splitter, 1);

    // 粘贴（含右键粘贴、拖放）后立刻排版 —— 只处理这一条路径，
    // 手动输入交给 JsonEditor 的自动缩进，不做整篇重排。
    // 走 lambda 而不是直接连成员函数：formatPasted 有带状态文案的默认参数，
    // 直接连的话信号（0 参）与槽（1 参）的参数个数检查在编译期就过不了。
    connect(m_input, &JsonEditor::textPasted, this, [this] { formatPasted(); });

    connect(formatBtn, &QPushButton::clicked, this, [this] { formatJson(true); });
    connect(compactBtn, &QPushButton::clicked, this, [this] { formatJson(false); });
    connect(escapeBtn, &QPushButton::clicked, this, [this] {
        m_input->setErrorLine(-1);
        m_output->setPlainText(jsontext::escapeText(m_input->fullText()));
        setStatus(QStringLiteral("已转义"), true);
    });
    connect(unescapeBtn, &QPushButton::clicked, this, [this] {
        m_input->setErrorLine(-1);
        m_output->setPlainText(jsontext::unescapeText(m_input->fullText()));
        setStatus(QStringLiteral("已反转义"), true);
    });
    connect(copyBtn, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_output->fullText());
        setStatus(QStringLiteral("已复制到剪贴板"), true);
    });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_input->clear();
        m_output->clear();
        m_input->setErrorLine(-1);
        setStatus(QString(), true);
    });
}

void JsonFmtPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 进入插件就把焦点放进输入框：不用先点一下，直接就能粘贴或打字。
    // 延后一拍：宿主是"先切页再设焦点"，同步设会被后面的焦点链覆盖。
    QTimer::singleShot(0, this, [this] {
        m_input->setFocus(Qt::OtherFocusReason);
    });
}

void JsonFmtPanel::setStatus(const QString &text, bool ok)
{
    m_status->setText(text);
    xtools::theme::applyStyle(m_status, QStringLiteral("font-size:12px;color:@%1;")
                                             .arg(xtools::theme::tokenName(
                                                 ok ? xtools::theme::Color::Success
                                                    : xtools::theme::Color::Danger)));
}

void JsonFmtPanel::replaceInputText(const QString &text, int cursorPos)
{
    // 用替换全文而不是 setPlainText：后者会清空撤销栈，用户按 Ctrl+Z 就回不去了。
    QTextCursor cursor(m_input->document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);

    QTextCursor restored(m_input->document());
    restored.setPosition(qBound(0, cursorPos, m_input->document()->characterCount() - 1));
    m_input->setTextCursor(restored);
}

void JsonFmtPanel::setInputText(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;

    // 走与"粘贴即格式化"同一条路径：替换全文（保留撤销栈）+ 整篇重排
    replaceInputText(text, static_cast<int>(text.size()));
    formatPasted(QStringLiteral("已载入首页搜索框里的 JSON"));
}

void JsonFmtPanel::formatPasted(const QString &okStatus)
{
    const QString text = m_input->fullText();
    if (text.trimmed().isEmpty())
        return;

    QJsonParseError err{};
    // 走 UTF-8：JSON 规范就是 UTF-8，QString::toUtf8 不会丢内容
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
        return;   // 粘进来的是片段或根本不是 JSON：保持原样，别自作主张

    const QString formatted = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    if (formatted == text) {
        setStatus(QStringLiteral("内容已是格式化后的样子"), true);
        return;
    }

    const int cursorPos =
        qBound(0, m_input->textCursor().position(), static_cast<int>(text.size()));
    replaceInputText(formatted, mapCursorPosition(text, cursorPos, formatted));
    setStatus(okStatus.isEmpty() ? QStringLiteral("已格式化粘贴内容") : okStatus, true);
}

void JsonFmtPanel::formatJson(bool indent)
{
    const QString text = m_input->fullText();
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral("先粘贴点 JSON 内容"), false);
        return;
    }

    QJsonParseError err{};
    // 走 UTF-8：JSON 规范就是 UTF-8，QString::toUtf8 不会丢内容
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        // 除了给文字说明，还要在编辑框里把出错行标出来：只有文字时用户得自己数行
        const int offset = qBound(0, err.offset, static_cast<int>(text.size()));
        m_input->setErrorLine(jsontext::lineOfOffset(text, offset));

        QTextCursor cursor = m_input->textCursor();
        cursor.setPosition(offset);
        m_input->setTextCursor(cursor);   // 光标跳过去，顺带滚动到出错位置

        setStatus(QStringLiteral("解析失败：%1").arg(jsontext::describeParseError(err, text)), false);
        return;
    }

    m_input->setErrorLine(-1);
    // 左边由"粘贴即格式化"和自动缩进负责，这里只把结果放进右侧结果区：
    // 一个按钮只干一件事，用户点它时看到的总是"结果区变了"。
    const QByteArray out = doc.toJson(indent ? QJsonDocument::Indented : QJsonDocument::Compact);
    m_output->setPlainText(QString::fromUtf8(out));
    setStatus(indent ? QStringLiteral("已格式化") : QStringLiteral("已压缩"), true);
}

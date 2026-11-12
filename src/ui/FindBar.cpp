#include "FindBar.h"

#include "ThemePalette.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/// 匹配数封顶。普通工具面板的文档远到不了这个量级；封顶是为了防住「往输出框里
/// 灌了一份几十 MB 的日志」这类极端输入 —— 每次按键都全文扫描会把界面拖死。
/// 到顶后计数显示「N+」，仍然可以继续查找（只是不再保证序号完整）。
constexpr int kMaxMatches = 9999;

/// 浮层距面板右缘 / 顶部的留白。插件面板自身的边距约 28px，取更小值让查找条
/// 看起来贴着面板边缘，而不是悬在内容中间。
constexpr int kBarMargin = 12;

/// 把控件指针解成「可搜索目标」的三个入口。QPlainTextEdit 与 QTextEdit 没有公共
/// 编辑接口基类（只有 QWidget），但操作都落在 QTextCursor / QTextDocument 上，
/// 这里集中做一次 qobject_cast 分发，调用侧不用关心具体是哪种控件。
struct TextTarget
{
    QPointer<QWidget> widget;   ///< QPlainTextEdit 或 QTextEdit，面板销毁时自动置空

    bool isValid() const { return !widget.isNull(); }

    QTextDocument *document() const
    {
        if (auto *plain = qobject_cast<QPlainTextEdit *>(widget))
            return plain->document();
        if (auto *rich = qobject_cast<QTextEdit *>(widget))
            return rich->document();
        return nullptr;
    }

    QTextCursor cursor() const
    {
        if (auto *plain = qobject_cast<QPlainTextEdit *>(widget))
            return plain->textCursor();
        if (auto *rich = qobject_cast<QTextEdit *>(widget))
            return rich->textCursor();
        return QTextCursor();
    }

    /// 选中匹配并滚动过去。两个控件类都有 setTextCursor/ensureCursorVisible，
    /// 语义一致（QPlainTextEdit 的 ensureCursorVisible 只保证光标块可见）。
    void setCursor(const QTextCursor &cursor) const
    {
        if (auto *plain = qobject_cast<QPlainTextEdit *>(widget)) {
            plain->setTextCursor(cursor);
            plain->ensureCursorVisible();
        } else if (auto *rich = qobject_cast<QTextEdit *>(widget)) {
            rich->setTextCursor(cursor);
            rich->ensureCursorVisible();
        }
    }
};

/// 判断控件（或其任一祖先到顶为止）是不是 QPlainTextEdit / QTextEdit。
/// 焦点通常落在编辑器的 viewport 上，所以要沿父链向上找一层。
TextTarget editorAt(QWidget *widget)
{
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (qobject_cast<QPlainTextEdit *>(w) || qobject_cast<QTextEdit *>(w))
            return TextTarget{w};
    }
    return TextTarget{};
}

} // namespace

void FindBar::attach(QWidget *panel)
{
    if (!panel)
        return;
    // 幂等：独立窗口接管主窗口面板时，面板随 reparent 把已有的查找条一起带走了，
    // 再挂一个会出现两条叠在一起（两条都是 qApp 过滤器，还会互相抢焦点）。
    if (panel->findChild<FindBar *>())
        return;
    new FindBar(panel);   // parent 即面板：面板销毁时查找条一起析构
}

FindBar::FindBar(QWidget *panel)
    : QWidget(panel)
    , m_panel(panel)
{
    setObjectName(QStringLiteral("findBar"));
    // 浮层不想要系统焦点框，键盘导航由自己的 eventFilter 处理
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_StyledBackground, true);   // qss 的 background 才会画在 QWidget 子类上

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 6, 8, 6);
    layout->setSpacing(6);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("查找内容"));
    m_input->setClearButtonEnabled(true);
    m_input->setFixedWidth(220);
    // 查找条内部拒绝 Ctrl+F 的默认行为没有意义，这里无需处理 —— 全部走 qApp 过滤器

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("findCount"));
    m_count->setMinimumWidth(64);
    m_count->setAlignment(Qt::AlignCenter);

    auto makeButton = [this](const QString &text, const QString &objectName,
                             const QString &tooltip) {
        auto *button = new QToolButton(this);
        button->setObjectName(objectName);
        button->setText(text);
        button->setToolTip(tooltip);
        button->setFixedSize(24, 24);
        button->setFocusPolicy(Qt::NoFocus);   // 回车跳转时焦点不能被按钮抢走
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    // 大小写开关默认「不区分」：与浏览器 / 编辑器的查找习惯一致
    m_caseBtn = makeButton(QStringLiteral("Aa"), QStringLiteral("findCase"),
                           QStringLiteral("区分大小写"));
    m_caseBtn->setCheckable(true);
    m_prevBtn = makeButton(QStringLiteral("\u2191"), QStringLiteral("findPrev"),
                           QStringLiteral("上一个（Shift+回车）"));
    m_nextBtn = makeButton(QStringLiteral("\u2193"), QStringLiteral("findNext"),
                           QStringLiteral("下一个（回车）"));
    m_closeBtn = makeButton(QStringLiteral("\u2715"), QStringLiteral("findClose"),
                            QStringLiteral("关闭（Esc）"));

    layout->addWidget(m_input);
    layout->addWidget(m_count);
    layout->addWidget(m_caseBtn);
    layout->addWidget(m_prevBtn);
    layout->addWidget(m_nextBtn);
    layout->addWidget(m_closeBtn);

    // 样式全走主题令牌（规则 7）：模板挂在 FindBar 上，换肤时 Theme 逐控件重算。
    // 输入框 / 按钮在全局 qss 里也有样式，这里的后代选择器优先级更高，会盖住全局值。
    xtools::theme::applyStyle(this, QStringLiteral(
        "#findBar { background: @card; border: 1px solid @border-strong;"
        "           border-radius: 8px; }"
        "#findBar QLineEdit { background: @bg; border: 1px solid @border-strong;"
        "                     border-radius: 6px; padding: 1px 6px; min-height: 22px; }"
        "#findBar QLineEdit:focus { border-color: @border-accent-strong; }"
        "#findBar QLabel { background: transparent; color: @text-second; }"
        "#findBar QLabel[state='empty'] { color: @text-faint; }"
        "#findBar QLabel[state='none'] { color: @danger-text; }"
        "#findBar QToolButton { background: transparent; border: none; border-radius: 4px;"
        "                       color: @text-second; font-size: 12px; }"
        "#findBar QToolButton:hover { background: @bg-hover; color: @text; }"
        "#findBar QToolButton:pressed { background: @bg-pressed; }"
        "#findBar QToolButton:checked { background: @accent-soft; color: @accent-text; }"
    ));

    connect(m_input, &QLineEdit::textChanged, this, [this] { refreshAndJump(); });
    connect(m_caseBtn, &QToolButton::toggled, this, [this] { refreshAndJump(); });
    connect(m_prevBtn, &QToolButton::clicked, this, [this] { step(-1); });
    connect(m_nextBtn, &QToolButton::clicked, this, [this] { step(1); });
    connect(m_closeBtn, &QToolButton::clicked, this, [this] { closeBar(); });

    // 面板尺寸变化时贴回右上角；面板显示时也一样（第一次 show 前宽度还是 0）
    m_panel->installEventFilter(this);

    eventFilterOnApp();

    hide();   // attach 时只是挂上，不显示
}

FindBar::~FindBar()
{
    // qApp 级过滤器必须显式摘除：析构时 m_panel 可能已经先没了，靠父子关系
    // 摘不掉挂在 qApp 上的这份（否则悬空过滤器会在下一次任意按键时崩溃）。
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeEventFilter(this);
}

void FindBar::eventFilterOnApp()
{
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    // ---- 面板几何变化：浮层贴回右上角（这是挂在 m_panel 上的那部分过滤）
    if (watched == m_panel) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
            reposition();
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() != QEvent::KeyPress)
        return QWidget::eventFilter(watched, event);

    auto *key = static_cast<QKeyEvent *>(event);
    const int key_value = key->key();
    const Qt::KeyboardModifiers mods = key->modifiers();

    // ---- Ctrl+F（macOS 上 ControlModifier 对应 Cmd）：焦点在面板子树内时唤出查找条。
    // 用 qApp 过滤器而不是面板级过滤器：按键永远先递给焦点控件，焦点在编辑器里时
    // 面板根本收不到这个事件。
    const bool findCombo = (key_value == Qt::Key_F)
        && (mods & (Qt::ControlModifier | Qt::MetaModifier))
        && (mods & ~(Qt::ControlModifier | Qt::MetaModifier | Qt::ShiftModifier)) == 0;
    if (findCombo) {
        QWidget *focus = QApplication::focusWidget();
        const bool insidePanel = focus && (focus == m_panel || m_panel->isAncestorOf(focus));
        // 焦点在查找条自己的输入框里（bar 已显示，再次 Ctrl+F）也算面板内
        if (insidePanel) {
            open();
            return true;   // 吞掉，不让编辑器把它当普通按键处理
        }
        return QWidget::eventFilter(watched, event);
    }

    // ---- 查找条输入框内的按键：回车跳转、Esc 关闭。只拦这几个键，
    // 其余（打字、粘贴、光标移动）原样放行给 QLineEdit。
    if (watched == m_input && isVisible()) {
        if (key_value == Qt::Key_Return || key_value == Qt::Key_Enter) {
            // Shift+回车 = 上一个，裸回车 = 下一个（与主流编辑器一致）
            step(mods & Qt::ShiftModifier ? -1 : 1);
            return true;
        }
        if (key_value == Qt::Key_Escape && mods == Qt::NoModifier) {
            closeBar();
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void FindBar::open()
{
    pickTarget(m_input->hasFocus());   // 焦点已在输入框里（二次 Ctrl+F）时保持现有目标
    if (!m_targetWidget)
        return;   // 面板里没有可搜索的文本控件：静默忽略，不弹一个空壳查找条

    if (!isVisible()) {
        show();
        reposition();
        raise();   // 插件面板可能后创建了一些子控件，浮层要保证在最上面
    }
    m_input->setFocus(Qt::ShortcutFocusReason);
    m_input->selectAll();
    refreshAndJump();
}

void FindBar::pickTarget(bool keepCurrent)
{
    // 二次 Ctrl+F（焦点在输入框）：不重选目标，避免焦点往返把用户正在看的编辑器换掉
    if (keepCurrent && m_targetWidget)
        return;

    // 焦点优先：焦点在哪个编辑器（或其 viewport）上就搜哪个。
    // 双编辑器面板（textkit 的对比、httpclient 的请求/响应）必须这样才符合直觉。
    QWidget *focus = QApplication::focusWidget();
    if (focus && focus != m_input) {
        const TextTarget fromFocus = editorAt(focus);
        if (fromFocus.isValid() && m_panel->isAncestorOf(fromFocus.widget)) {
            setTarget(fromFocus.widget);
            return;
        }
    }

    // 兜底：面板里第一个可见的文本编辑控件（按 findChildren 的树序）。
    // 焦点停在按钮/下拉框上时，用户预期的就是「搜面板里那块内容」。
    const auto plains = m_panel->findChildren<QPlainTextEdit *>();
    for (QPlainTextEdit *plain : plains) {
        if (plain->isVisible()) {
            setTarget(plain);
            return;
        }
    }
    const auto riches = m_panel->findChildren<QTextEdit *>();
    for (QTextEdit *rich : riches) {
        if (rich->isVisible()) {
            setTarget(rich);
            return;
        }
    }
    setTarget(nullptr);
}

void FindBar::setTarget(QWidget *target)
{
    const TextTarget resolved{target};
    // 换目标先断开旧文档的监听：QTextDocument 不受 QPointer 保护，靠这个断连
    // 保证不悬挂（面板销毁时整条 FindBar 一起析构，这里只需处理「换目标」）。
    if (m_docChanged)
        disconnect(m_docChanged);
    m_targetWidget = resolved.widget;
    m_positions.clear();
    m_needle.clear();

    if (!m_targetWidget)
        return;

    // 编辑器内容被插件/用户改掉时只刷新计数，不跳光标 ——
    // 打断用户正在输入的位置是很差的手感。
    if (QTextDocument *doc = resolved.document())
        m_docChanged = connect(doc, &QTextDocument::contentsChanged, this, [this] {
            refreshMatches();
            updateCountLabel(-1);
        });
}

void FindBar::refreshMatches()
{
    m_positions.clear();
    m_needle.clear();
    QTextDocument *doc = TextTarget{m_targetWidget}.document();
    const QString needle = m_input->text();
    if (!doc || needle.isEmpty())
        return;

    m_needle = needle;
    // 默认不区分大小写；勾了 Aa 才走 FindCaseSensitively
    const QTextDocument::FindFlags flags =
        m_caseBtn->isChecked() ? QTextDocument::FindCaseSensitively : QTextDocument::FindFlags();

    int from = 0;
    const int charCount = doc->characterCount();
    while (from <= charCount && m_positions.size() < kMaxMatches) {
        const QTextCursor match = doc->find(needle, from, flags);
        if (match.isNull())
            break;
        m_positions.append(match.selectionStart());
        // 从匹配末尾继续：同一位置不会被重复收集（匹配长度 >= 1，不会死循环）
        from = match.selectionEnd();
    }
}

void FindBar::refreshAndJump()
{
    refreshMatches();
    if (m_positions.isEmpty()) {
        updateCountLabel(-1);
        return;
    }

    // 跳到「光标位置起的第一处」，没有就回绕到第一处（与浏览器查找一致）
    const int cursorPos = TextTarget{m_targetWidget}.cursor().selectionStart();
    int index = -1;
    for (int i = 0; i < m_positions.size(); ++i) {
        if (m_positions.at(i) >= cursorPos) {
            index = i;
            break;
        }
    }
    if (index < 0)
        index = 0;
    jumpTo(index);
}

void FindBar::step(int direction)
{
    if (!m_targetWidget)
        return;
    refreshMatches();
    if (m_positions.isEmpty()) {
        updateCountLabel(-1);
        return;
    }

    const QTextCursor current = TextTarget{m_targetWidget}.cursor();
    int index = -1;
    if (direction > 0) {
        // 下一个：从「当前选区结束」起向后找。选中某处匹配后光标就在它开头，
        // 用 selectionEnd 才能跳到自己而不原地不动。
        const int from = current.hasSelection() ? current.selectionEnd() : current.selectionStart();
        for (int i = 0; i < m_positions.size(); ++i) {
            if (m_positions.at(i) >= from) {
                index = i;
                break;
            }
        }
        if (index < 0)
            index = 0;   // 回绕到开头
    } else {
        // 上一个：取起点在当前选区开始之前的最后一处；没有则回绕到最后一处
        const int from = current.selectionStart();
        for (int i = m_positions.size() - 1; i >= 0; --i) {
            if (m_positions.at(i) < from || (m_positions.at(i) == from && !current.hasSelection())) {
                index = i;
                break;
            }
        }
        if (index < 0)
            index = m_positions.size() - 1;
    }
    jumpTo(index);
}

void FindBar::jumpTo(int index)
{
    if (index < 0 || index >= m_positions.size())
        return;
    const int start = m_positions.at(index);
    QTextCursor cursor(TextTarget{m_targetWidget}.document());
    cursor.setPosition(start);
    cursor.setPosition(start + m_needle.length(), QTextCursor::KeepAnchor);
    TextTarget{m_targetWidget}.setCursor(cursor);
    updateCountLabel(index);
}

void FindBar::updateCountLabel(int index)
{
    QString text;
    QString state;
    if (m_input->text().isEmpty()) {
        text = QStringLiteral("输入关键词");
        state = QStringLiteral("empty");
    } else if (m_positions.isEmpty()) {
        text = QStringLiteral("无结果");
        state = QStringLiteral("none");
    } else {
        const int clamped = qBound(0, index, m_positions.size() - 1);
        text = QStringLiteral("%1/%2").arg(clamped + 1).arg(m_positions.size());
        if (m_positions.size() >= kMaxMatches)
            text += QLatin1Char('+');   // 匹配数封顶：序号仍然有效，只是总数不全
        state = QStringLiteral("normal");
    }
    m_count->setText(text);
    // 动态属性 + repolish：qss 的属性选择器要重新求值才会换色（与磁贴 hover 同一套手法）
    m_count->setProperty("state", state);
    m_count->style()->unpolish(m_count);
    m_count->style()->polish(m_count);
}

void FindBar::closeBar()
{
    hide();
    // 焦点还给编辑器：关闭后继续打字应当落在原来正在编辑的地方
    if (TextTarget{m_targetWidget}.isValid())
        TextTarget{m_targetWidget}.widget->setFocus();
}

void FindBar::reposition()
{
    // 子控件不会像顶层窗口那样在 show() 时自动按 sizeHint 撑开 —— 不调这个，
    // 查找条会停在 QWidget 默认的 ~100x30，输入框、计数、按钮全部叠在一起。
    adjustSize();
    QWidget *parent = parentWidget();
    if (!parent)
        return;
    move(qMax(0, parent->width() - width() - kBarMargin), kBarMargin);
}

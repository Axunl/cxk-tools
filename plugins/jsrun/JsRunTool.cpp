#include "JsRunTool.h"

#include "JsEngineRunner.h"
#include "JsRunConsole.h"
#include "JsRunSamples.h"
#include "JsRunStore.h"
#include "ThemePalette.h"
#include "LayoutMetrics.h" // 颜色令牌：写死色值在深色主题下会留白块

#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>

namespace {

/// 脚本长度上限。引擎本身不限，但这个输入框不是拿来存大文件的：超过 200 KB
/// 既改不动也看不清，而「上次脚本」还会被写进 ini。超限时直接拒绝运行并说明原因。
constexpr int kMaxScriptChars = 200 * 1024;

/// 编辑器改动的落盘防抖。逐键写盘没有收益，只会让 ini 的修改时间疯狂跳动。
constexpr int kSaveDebounceMs = 800;

/// 复制按钮「已复制」提示的停留时长。
constexpr int kCopyHintMs = 1200;

/// 下拉框右侧的自绘小三角。
///
/// 为什么必须自绘：全局 style.qss 为了消掉原生 QComboBox 在圆角外框上甩出的深色
/// L 形边框，给 `::drop-down` 写了规则，副作用是 Qt 连默认箭头也不再画。漏补时
/// 「示例片段 / 最近运行」看起来只是两行静态文字，用户根本不知道能展开。
/// 位置由 JsRunPanel::eventFilter 在 Resize / Show 时贴合到 qss 留出的右侧区域。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("jsrunComboArrow"));
        setFixedSize(10, 8);
        // 点三角要能穿透给下拉框，否则点了不弹出候选
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        // 跟主题走：写死浅灰在深色底上几乎看不见
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

/// 日志文本转 HTML：转义 `& < >`，再把换行变 <br/>（appendHtml 不认 \n）。
QString escapeLogText(const QString &text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
}

/// 取第一行并截断，用于状态行——错误详情可能有好几行栈帧，状态行只放得下一行。
QString firstLineCapped(const QString &text, int maxChars = 90)
{
    QString line = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (line.size() > maxChars)
        line = line.left(maxChars) + QStringLiteral("…");
    return line;
}

} // namespace

// ---------------------------------------------------------------- 工具面板

/// 脚本运行面板：上方编辑器，下方输出（console 日志 + 结果 + 耗时）。
///
/// 线程相关的所有对象（worker / RunContext / 超时定时器）都由面板独占，
/// 面板销毁时必须先打断执行再放行，见析构函数的注释。
class JsRunPanel : public QWidget
{
    Q_OBJECT

public:
    explicit JsRunPanel(QWidget *parent = nullptr);
    ~JsRunPanel() override;

protected:
    /// 只用来给两个下拉框的自绘三角摆位（跟着 Resize / 启用状态走）。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();

    /// 首次进入：有「上次脚本」就回填，否则载入第一条示例。
    void loadInitialScript();
    /// 重建「最近运行」下拉（内容来自 ini）。
    void refreshHistory();

    void setEditorText(const QString &code);
    void startRun();
    void onTimeout();
    void appendLog(const QString &level, const QString &text);
    void onRunFinished(const jsrun::RunResult &result);

    /// 追加一个结果块（标题 + 正文）。标题按语义色上色，正文保持默认前景色。
    void appendBlock(const QString &title, const QString &body, xtools::theme::Color token);
    void setStatus(const QString &text, xtools::theme::Color token);
    void copyOutput();
    void clearOutput();
    void persistScript();

    QPlainTextEdit *m_editor = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QComboBox *m_samples = nullptr;
    QComboBox *m_history = nullptr;
    QPushButton *m_run = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_clear = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_timeout = nullptr;
    QTimer *m_saveDelay = nullptr;

    jsrun::Store m_store;

    /// 用 QPointer：worker 是 QThread，线程结束时由 finished → deleteLater 自删，
    /// 比面板活得久或先死都不一定，裸指针会悬空。
    QPointer<jsrun::JsRunWorker> m_worker;
    std::shared_ptr<jsrun::RunContext> m_ctx;

    bool m_running = false;
    /// 正在运行的那份代码（历史记录用：运行期间用户可能已经在编辑器里改了）
    QString m_runningCode;
    int m_logLines = 0;
    bool m_logOverflow = false;
};

JsRunPanel::JsRunPanel(QWidget *parent)
    : QWidget(parent)
    , m_store(jsrun::iniPath())
{
    buildUi();
    loadInitialScript();
    setStatus(QStringLiteral("就绪 · 脚本只在本地沙箱执行，不会访问文件、网络或剪贴板"),
              xtools::theme::Color::TextFaint);
}

JsRunPanel::~JsRunPanel()
{
    m_timeout->stop();
    m_saveDelay->stop();
    persistScript(); // 关面板前兜底保存：防抖定时器可能还没到点

    // 先打断，再等待。顺序不能反：死循环不打断的话 wait() 一定超时。
    if (m_ctx) {
        m_ctx->timedOut.store(true);
        m_ctx->interrupt();
    }

    if (m_worker && !m_worker->isFinished()) {
        if (!m_worker->wait(jsrun::kShutdownWaitMs)) {
            // 极端情况：引擎卡在一次原生调用里（超大字符串/正则回溯），中断暂时打
            // 不进去。这里既不 terminate（会撕掉 V4 的堆）也不 delete 正在跑的
            // QThread（Qt 会直接崩），而是脱钩——线程对象在 finished 时自删，
            // RunContext 由 shared_ptr 保活，worker 不会碰到已销毁的面板。
            qWarning() << "jsrun: 执行线程未在" << jsrun::kShutdownWaitMs
                       << "ms 内退出，改为异步回收";
        }
    }
}

bool JsRunPanel::eventFilter(QObject *watched, QEvent *event)
{
    auto *combo = qobject_cast<QComboBox *>(watched);
    if (combo) {
        auto *arrow = combo->findChild<QWidget *>(QStringLiteral("jsrunComboArrow"));
        if (arrow) {
            switch (event->type()) {
            case QEvent::Resize:
            case QEvent::Show:
                arrow->move(combo->width() - 14, (combo->height() - arrow->height()) / 2);
                break;
            case QEvent::EnabledChange:
                arrow->update();
                break;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------- 界面搭建

void JsRunPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐，与其余插件面板同一套留白约定
    root->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    root->setSpacing(10);

    auto *hint = new QLabel(
        QStringLiteral("JavaScript · 运行在后台线程，超过 %1 秒自动中断；没有文件 / 网络 / 剪贴板能力")
            .arg(jsrun::kTimeoutMs / 1000),
        this);
    xtools::theme::applyStyle(hint, QStringLiteral("font-size:12px;color:@text-faint;"));
    // 窄窗口下宁可裁掉说明，也不要用它的宽度把整个面板的最小宽度顶大
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    root->addWidget(hint);

    // ---- 工具行：示例 / 历史 / 复制 / 清空 / 运行
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    auto addComboArrow = [this](QComboBox *combo) {
        auto *arrow = new ComboArrow(combo);
        arrow->move(combo->width() - 14, (combo->height() - arrow->height()) / 2);
        combo->installEventFilter(this);
    };

    m_samples = new QComboBox(this);
    m_samples->addItem(QStringLiteral("示例片段…"));
    const QVector<jsrun::Sample> allSamples = jsrun::samples();
    for (const jsrun::Sample &sample : allSamples)
        m_samples->addItem(sample.title);
    m_samples->setMinimumWidth(180);
    m_samples->setToolTip(QStringLiteral("载入一段内置示例，直接把编辑器内容顶掉（可用 Ctrl+Z 撤回）"));
    toolbar->addWidget(m_samples);
    addComboArrow(m_samples);

    m_history = new QComboBox(this);
    m_history->setMinimumWidth(180);
    m_history->setToolTip(QStringLiteral("这里只收成功运行过的脚本，最新的一条排在最前"));
    toolbar->addWidget(m_history);
    addComboArrow(m_history);

    toolbar->addStretch(1);

    m_copy = new QPushButton(QStringLiteral("复制输出"), this);
    toolbar->addWidget(m_copy);

    m_clear = new QPushButton(QStringLiteral("清空输出"), this);
    toolbar->addWidget(m_clear);

    m_run = new QPushButton(QStringLiteral("运行"), this);
    m_run->setProperty("accent", true); // 全局 qss 认这个属性 = 强调色
    m_run->setToolTip(QStringLiteral("运行脚本（Ctrl+Enter）"));
    toolbar->addWidget(m_run);

    root->addLayout(toolbar);

    // ---- 编辑器
    m_editor = new QPlainTextEdit(this);
    m_editor->setObjectName(QStringLiteral("jsrunEditor"));
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(4.0 * QFontMetricsF(m_editor->font()).horizontalAdvance(QLatin1Char(' ')));
    m_editor->setPlaceholderText(QStringLiteral("在这里写 JavaScript，Ctrl+Enter 运行"));
    m_editor->setMinimumHeight(150);
    xtools::theme::applyStyle(m_editor,
                              QStringLiteral("QPlainTextEdit{background:@bg;border:1px solid @border;"
                                             "border-radius:8px;padding:8px;color:@text;}"
                                             "QPlainTextEdit:focus{border-color:@border-accent-strong;}"));
    root->addWidget(m_editor, 3);

    // ---- 输出
    auto *outputCaption = new QLabel(QStringLiteral("输出"), this);
    xtools::theme::applyStyle(outputCaption,
                              QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    root->addWidget(outputCaption);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("jsrunOutput"));
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setReadOnly(true);
    // 最后一道防线：日志条数在 appendLog 里已经限过，这里再限块数，
    // 避免异常情况下 QTextDocument 无限长把内存吃穿
    m_output->setMaximumBlockCount(4000);
    m_output->setPlaceholderText(QStringLiteral("运行后这里显示 console 输出、返回值与耗时"));
    m_output->setMinimumHeight(90);
    // @bg-alt：只读区用下沉底色，与编辑器区分开
    xtools::theme::applyStyle(m_output,
                              QStringLiteral("QPlainTextEdit{background:@bg-alt;border:1px solid @border;"
                                             "border-radius:8px;padding:8px;color:@text-body;}"));
    root->addWidget(m_output, 2);

    // ---- 状态行
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    root->addWidget(m_status);

    // ---- 信号
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, &JsRunPanel::onTimeout);

    m_saveDelay = new QTimer(this);
    m_saveDelay->setSingleShot(true);
    m_saveDelay->setInterval(kSaveDebounceMs);
    connect(m_saveDelay, &QTimer::timeout, this, &JsRunPanel::persistScript);

    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] { m_saveDelay->start(); });
    connect(m_run, &QPushButton::clicked, this, &JsRunPanel::startRun);
    connect(m_copy, &QPushButton::clicked, this, &JsRunPanel::copyOutput);
    connect(m_clear, &QPushButton::clicked, this, &JsRunPanel::clearOutput);

    // Ctrl+Enter：用 QShortcut 而不是 eventFilter 拦回车——编辑器是多行的，
    // 裸回车必须留给换行。ReceivesReturn 与小键盘 Enter 两种写法都接。
    auto *runShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), this);
    runShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(runShortcut, &QShortcut::activated, this, &JsRunPanel::startRun);
    auto *runShortcutKeypad = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Enter")), this);
    runShortcutKeypad->setContext(Qt::WidgetWithChildrenShortcut);
    connect(runShortcutKeypad, &QShortcut::activated, this, &JsRunPanel::startRun);

    connect(m_samples, &QComboBox::activated, this, [this, allSamples](int index) {
        if (index > 0 && index - 1 < allSamples.size()) {
            const jsrun::Sample &sample = allSamples.at(index - 1);
            setEditorText(sample.code);
            setStatus(QStringLiteral("已载入示例：%1").arg(sample.title),
                      xtools::theme::Color::TextFaint);
        }
        // 复位成占位项，否则第二次选同一条不会再触发 activated
        QSignalBlocker blocker(m_samples);
        m_samples->setCurrentIndex(0);
    });

    connect(m_history, &QComboBox::activated, this, [this](int index) {
        if (index <= 0)
            return;
        const QString code = m_history->itemData(index).toString();
        if (!code.isEmpty()) {
            setEditorText(code);
            setStatus(QStringLiteral("已载入历史片段（第 %1 条）").arg(index),
                      xtools::theme::Color::TextFaint);
        }
        QSignalBlocker blocker(m_history);
        m_history->setCurrentIndex(0);
    });
}

// ---------------------------------------------------------------- 脚本与历史

void JsRunPanel::loadInitialScript()
{
    const QString saved = m_store.lastScript();
    setEditorText(saved.isEmpty() ? jsrun::firstSampleCode() : saved);
    refreshHistory();
}

void JsRunPanel::setEditorText(const QString &code)
{
    // 不用 setPlainText：那会清空撤销栈，用户误点示例后连 Ctrl+Z 都救不回来
    m_editor->selectAll();
    m_editor->insertPlainText(code);

    // 光标落到第一行：长脚本从中间开始看会很莫名其妙
    QTextCursor cursor = m_editor->textCursor();
    cursor.movePosition(QTextCursor::Start);
    m_editor->setTextCursor(cursor);
    m_editor->ensureCursorVisible();
}

void JsRunPanel::refreshHistory()
{
    QSignalBlocker blocker(m_history);
    m_history->clear();
    m_history->addItem(QStringLiteral("最近运行…"));

    const QStringList items = m_store.history();
    for (const QString &code : items) {
        const int lines = code.count(QLatin1Char('\n')) + 1;
        QString title = code.section(QLatin1Char('\n'), 0, 0).trimmed();
        if (title.isEmpty())
            title = QStringLiteral("（空行开头）");
        if (title.size() > 32)
            title = title.left(32) + QStringLiteral("…");
        m_history->addItem(QStringLiteral("%1（%2 行）").arg(title).arg(lines), code);
    }
    m_history->setCurrentIndex(0);
    // 一条历史都没有时禁用，避免用户点开一个只有占位项的空列表
    m_history->setEnabled(!items.isEmpty());
}

void JsRunPanel::persistScript()
{
    m_store.setLastScript(m_editor->toPlainText());
}

// ---------------------------------------------------------------- 执行

void JsRunPanel::startRun()
{
    if (m_running)
        return;

    const QString code = m_editor->toPlainText();
    if (code.trimmed().isEmpty()) {
        setStatus(QStringLiteral("先在编辑器里写点 JavaScript"), xtools::theme::Color::Danger);
        m_editor->setFocus();
        return;
    }
    if (code.size() > kMaxScriptChars) {
        setStatus(QStringLiteral("脚本过长（上限 %1 KB），请拆分后再运行").arg(kMaxScriptChars / 1024),
                  xtools::theme::Color::Danger);
        return;
    }

    m_running = true;
    m_runningCode = code;
    m_logLines = 0;
    m_logOverflow = false;
    m_output->clear();
    m_run->setEnabled(false);
    setStatus(QStringLiteral("执行中…（超过 %1 秒会自动中断）").arg(jsrun::kTimeoutMs / 1000),
              xtools::theme::Color::TextMuted);

    // 点运行就落盘：即便接下来超时或被切走，用户回来还能看到自己写的代码
    m_store.setLastScript(code);

    m_ctx = std::make_shared<jsrun::RunContext>();
    auto *worker = new jsrun::JsRunWorker(code, m_ctx);
    m_worker = worker;

    // 明确写 QueuedConnection：worker 的 run() 在新线程里跑，日志必须排队回主线程，
    // 绝不能在那个线程里碰控件。Auto 其实也会排队，这里写死意图防止后人改坏。
    connect(worker, &jsrun::JsRunWorker::logLine, this, &JsRunPanel::appendLog,
            Qt::QueuedConnection);
    connect(worker, &jsrun::JsRunWorker::finished, this, &JsRunPanel::onRunFinished,
            Qt::QueuedConnection);

    // 线程对象自删：面板可能先于线程销毁，此时不能再由面板 delete 它
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, worker] {
        if (m_worker == worker)
            m_worker = nullptr;
    });

    m_timeout->start(jsrun::kTimeoutMs);
    worker->start();
}

void JsRunPanel::onTimeout()
{
    if (!m_running || !m_ctx)
        return;

    // 先置位再打断：worker 看到 timedOut 才知道这次错误是「被我们打断」，
    // 否则会把它当成脚本自己的错误报给用户。
    m_ctx->timedOut.store(true);
    m_ctx->interrupt();
    setStatus(QStringLiteral("执行超时（>%1 秒），已中断").arg(jsrun::kTimeoutMs / 1000),
              xtools::theme::Color::WarnStrong);
}

void JsRunPanel::appendLog(const QString &level, const QString &text)
{
    // 循环里 log 的脚本能在一秒内产出上万条，全部排队到主线程会把界面拖死
    if (m_logOverflow)
        return;
    if (m_logLines >= jsrun::kMaxConsoleLines) {
        m_logOverflow = true;
        m_output->appendPlainText(
            QStringLiteral("…（console 输出超过 %1 条，后续内容已省略）").arg(jsrun::kMaxConsoleLines));
        return;
    }
    ++m_logLines;

    const xtools::theme::Color token = level == QLatin1String("warn")   ? xtools::theme::Color::WarnStrong
                                     : level == QLatin1String("error") ? xtools::theme::Color::Danger
                                                                       : xtools::theme::Color::TextBody;
    // 颜色在追加时取当前主题值。面板切走即销毁，不存在「换肤后颜色停住」的问题，
    // 所以这里用 appendHtml 而不是 applyStyle（后者服务于长期存在的控件）。
    m_output->appendHtml(QStringLiteral("<span style=\"color:%1;\">%2</span>")
                             .arg(xtools::theme::hex(token), escapeLogText(text)));
}

void JsRunPanel::onRunFinished(const jsrun::RunResult &result)
{
    m_timeout->stop();
    m_running = false;
    m_run->setEnabled(true);

    switch (result.status) {
    case jsrun::RunStatus::Ok:
        appendBlock(QStringLiteral("结果 · %1 ms").arg(result.elapsedMs), result.valueText,
                    xtools::theme::Color::AccentText);
        setStatus(QStringLiteral("完成 · %1 ms").arg(result.elapsedMs),
                  xtools::theme::Color::Success);
        // 只记成功的片段：失败脚本回填的价值低，还会把有用的挤掉
        m_store.addHistory(m_runningCode);
        refreshHistory();
        break;

    case jsrun::RunStatus::Error:
        appendBlock(QStringLiteral("错误 · %1 ms").arg(result.elapsedMs), result.errorText,
                    xtools::theme::Color::Danger);
        setStatus(firstLineCapped(result.errorText), xtools::theme::Color::Danger);
        break;

    case jsrun::RunStatus::Timeout:
        appendBlock(QStringLiteral("已中断"), result.errorText, xtools::theme::Color::WarnStrong);
        // 状态行在 onTimeout 里已经写过，这里不再覆盖成别的颜色
        break;
    }
}

void JsRunPanel::appendBlock(const QString &title, const QString &body, xtools::theme::Color token)
{
    if (!m_output->document()->isEmpty())
        m_output->appendPlainText(QString()); // 与日志区隔开一段
    m_output->appendHtml(QStringLiteral("<span style=\"color:%1;font-weight:600;\">%2</span>")
                             .arg(xtools::theme::hex(token), escapeLogText(title)));
    m_output->appendPlainText(body);
}

// ---------------------------------------------------------------- 杂项

void JsRunPanel::setStatus(const QString &text, xtools::theme::Color token)
{
    // 传令牌而不是色值：换肤时宿主按令牌重算，颜色跟着主题走
    xtools::theme::applyStyle(m_status, QStringLiteral("font-size:12px;color:@%1;")
                                             .arg(xtools::theme::tokenName(token)));
    m_status->setText(text);
}

void JsRunPanel::copyOutput()
{
    const QString text = m_output->toPlainText();
    if (text.isEmpty()) {
        setStatus(QStringLiteral("还没有可复制的内容"), xtools::theme::Color::TextFaint);
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    m_copy->setText(QStringLiteral("已复制"));
    // 带接收者的 singleShot：面板先销毁时定时器随之作废，不会砸野指针
    QTimer::singleShot(kCopyHintMs, m_copy, [this] { m_copy->setText(QStringLiteral("复制输出")); });
}

void JsRunPanel::clearOutput()
{
    m_output->clear();
    m_logLines = 0;
    m_logOverflow = false;
    setStatus(QStringLiteral("已清空输出，编辑器内容保留"), xtools::theme::Color::TextFaint);
}

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo JsRunPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("jsrun");
    info.name = QStringLiteral("JS 代码运行");
    info.keywords = QStringList{QStringLiteral("js"), QStringLiteral("javascript"),
                                QStringLiteral("jsrun"), QStringLiteral("脚本"),
                                QStringLiteral("执行"), QStringLiteral("代码"),
                                QStringLiteral("eval")};
    info.description = QStringLiteral("在本地沙箱里运行 JavaScript，带控制台与 3 秒超时中断");
    // iconPath 留空 → 宿主用 IconBadge 生成字母徽章，不需要额外资源文件
    return info;
}

QWidget *JsRunPlugin::createPanel(QWidget *parent)
{
    // 每次进入工具都调用这里，必须返回全新面板（宿主不缓存）
    return new JsRunPanel(parent);
}

// .cpp 里有带 Q_OBJECT 的类（JsRunPanel / ComboArrow 没有），需要 moc 产物
#include "JsRunTool.moc"

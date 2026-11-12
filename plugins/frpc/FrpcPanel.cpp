#include "FrpcPanel.h"

#include "FrpcRunner.h"
#include "LayoutMetrics.h"
#include "ThemePalette.h"

#include <QAbstractSocket>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QTcpSocket>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

namespace {

using xtools::theme::Color;

/// 刷新节拍。600ms 对「看日志 / 看运行时长」都够快，又不会让界面一直在动。
/// 日志另外还挂了 Service::outputArrived 信号，所以真正的输出是即时的，
/// 这个定时器只负责「进程状态、运行时长、以及信号漏掉时的兜底」。
constexpr int kTickIntervalMs = 600;
constexpr int kSaveDebounceMs = 700;
constexpr int kValidateDebounceMs = 300;

/// 日志区最多显示多少行。再往上翻没人看，但 QTextEdit 的排版成本随行数线性增长，
/// 挂着几天不关面板会明显变卡。
constexpr int kMaxViewLines = 2000;

/// 重新进入面板时回填的历史日志行数（Service 缓冲区里可能更多）。
constexpr int kRestoreTailLines = 300;

/// 下拉框右侧的小三角。
///
/// 全局 qss 接管了 `QComboBox::drop-down`（为了消掉原生边框甩到圆角外的问题），
/// 副作用是 Qt 不再画箭头 —— 不补这一下，用户看不出那是可展开的下拉框。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(10, 8);
        setAttribute(Qt::WA_TransparentForMouseEvents);   // 点击穿透给下拉框
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        // 跟主题走：写死浅灰在深色底上几乎看不见
        painter.setBrush(isEnabled() ? xtools::theme::color(Color::TextFaint)
                                     : xtools::theme::color(Color::TextDisabled));
        QPainterPath path;
        path.moveTo(0.5, 2.0);
        path.lineTo(9.5, 2.0);
        path.lineTo(5.0, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
    }
};

/// 自带箭头、箭头位置跟随尺寸的下拉框（与 HttpPanel / ProcMonPanel 同一套做法）。
class ArrowCombo : public QComboBox
{
public:
    explicit ArrowCombo(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        m_arrow = new ComboArrow(this);
        m_arrow->show();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QComboBox::resizeEvent(event);
        placeArrow();
    }

    void showEvent(QShowEvent *event) override
    {
        QComboBox::showEvent(event);
        placeArrow();
    }

    void changeEvent(QEvent *event) override
    {
        QComboBox::changeEvent(event);
        if (event->type() == QEvent::EnabledChange && m_arrow)
            m_arrow->setEnabled(isEnabled());
    }

private:
    /// qss 在右侧留了 18px 给箭头，10px 的三角在其中居中。
    void placeArrow()
    {
        if (m_arrow)
            m_arrow->move(width() - 18 + 4, (height() - m_arrow->height()) / 2);
    }

    ComboArrow *m_arrow = nullptr;
};

/// 小节标题：统一灰蓝色 12px（与 jwt / procmon 的 caption 一致）。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    return label;
}

/// 等宽编辑器外观。颜色写 @令牌而非字面量：插件是独立动态库，
/// 宿主换肤时只能靠这份模板重算（见 ThemePalette.h 的 applyStyle）。
void styleCodeEditor(QPlainTextEdit *edit)
{
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    xtools::theme::applyStyle(edit, QStringLiteral(
        "QPlainTextEdit{background:@bg;border:1px solid @border;border-radius:8px;"
        "padding:8px 10px;color:@text;selection-background-color:@accent-soft;}"));
}

/// 日志区外观：底色用 @bg-alt 与配置区（@bg）做出层次，读起来不糊成一片。
void styleLogView(QTextEdit *view)
{
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    xtools::theme::applyStyle(view, QStringLiteral(
        "QTextEdit{background:@bg-alt;border:1px solid @border;border-radius:8px;"
        "padding:6px 8px;color:@text-body;selection-background-color:@accent-soft;}"));
}

/// 日志级别 → 颜色令牌。
Color logToken(frpctext::LogKind kind)
{
    switch (kind) {
    case frpctext::LogKind::Error:
        return Color::DangerText;
    case frpctext::LogKind::Warn:
        return Color::WarnText;
    case frpctext::LogKind::Success:
        return Color::SuccessText;
    case frpctext::LogKind::Info:
        break;
    }
    return Color::TextBody;
}

/// 一行说明文字（状态卡下方那条）。
void styleHint(QLabel *label)
{
    label->setWordWrap(false);
    // Ignored 横向策略：长错误信息不会把整个面板撑宽，超出部分裁掉并放进 tooltip
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
}

} // namespace

FrpcPanel::FrpcPanel(QWidget *parent)
    : QWidget(parent)
{
    m_profiles = m_store.ensureDefaults();
    int active = m_store.activeIndex();
    if (active < 0 || active >= m_profiles.size())
        active = 0;

    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐（宿主统一常量，别写死数字）
    layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    layout->setSpacing(10);

    layout->addWidget(buildToolbar());
    layout->addWidget(buildStatusCard());

    m_hint = new QLabel(this);
    styleHint(m_hint);
    layout->addWidget(m_hint);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(buildConfigArea());
    m_splitter->addWidget(buildLogArea());
    // 配置区给多一点：用户动手改的是它，日志多数时候只是扫一眼
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    layout->addWidget(m_splitter, 1);

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(kTickIntervalMs);
    connect(m_tickTimer, &QTimer::timeout, this, [this] {
        refreshState();
        pumpLog();
    });

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(kSaveDebounceMs);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { saveCurrent(false); });

    m_validateTimer = new QTimer(this);
    m_validateTimer->setSingleShot(true);
    m_validateTimer->setInterval(kValidateDebounceMs);
    connect(m_validateTimer, &QTimer::timeout, this, &FrpcPanel::refreshValidation);

    // 服务是模块级单例，可能在我们创建之前就已经在跑（用户切页再回来）
    auto *service = frpc::Service::instance();
    connect(service, &frpc::Service::stateChanged, this, &FrpcPanel::onServiceStateChanged);
    connect(service, &frpc::Service::outputArrived, this, &FrpcPanel::pumpLog);

    // Ctrl+S 保存、Ctrl+Enter 启动/停止。用 WidgetWithChildren 作用域，
    // 免得与主窗口或其他面板的同名快捷键打架。
    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    saveShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(saveShortcut, &QShortcut::activated, this, [this] {
        saveCurrent(true);
        setHint(QStringLiteral("配置已保存"), Color::SuccessText);
    });

    auto *runShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    runShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(runShortcut, &QShortcut::activated, this, &FrpcPanel::toggleRun);

    reloadProfileCombo(active);

    // 恢复现场：如果上一份面板关掉时 frpc 还在跑，这里把最近的日志接上。
    m_logLines.clear();
    appendLogView(service->tail(kRestoreTailLines));
    m_logCursor = service->lineCount();

    refreshState();
}

FrpcPanel::~FrpcPanel()
{
    // 面板要走：把当前编辑内容落盘（宿主随时会销毁面板，不存就丢）。
    // **不碰 frpc 进程** —— 那是 Service 的事，切页不能断穿透。
    saveCurrent(true);
}

QWidget *FrpcPanel::buildToolbar()
{
    auto *bar = new QWidget(this);
    auto *rows = new QVBoxLayout(bar);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(8);

    // ---- 第一行：配置档
    auto *line1 = new QHBoxLayout;
    line1->setSpacing(8);
    line1->addWidget(makeCaption(QStringLiteral("配置"), bar));

    m_profileCombo = new ArrowCombo(bar);
    m_profileCombo->setMinimumWidth(160);
    m_profileCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    line1->addWidget(m_profileCombo, 1);

    const auto makeSmallButton = [bar](const QString &text) {
        auto *button = new QPushButton(text, bar);
        button->setProperty("compact", true);   // 清掉全局 min-height，否则会撑高整行
        return button;
    };
    m_newButton = makeSmallButton(QStringLiteral("新建"));
    m_copyButton = makeSmallButton(QStringLiteral("复制"));
    m_renameButton = makeSmallButton(QStringLiteral("重命名"));
    m_deleteButton = makeSmallButton(QStringLiteral("删除"));
    line1->addWidget(m_newButton);
    line1->addWidget(m_copyButton);
    line1->addWidget(m_renameButton);
    line1->addWidget(m_deleteButton);
    rows->addLayout(line1);

    connect(m_profileCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || index == m_currentIndex)
            return;
        saveCurrent(true);   // 先把手上的改动存进**原来那份**
        loadProfile(index);
    });
    connect(m_newButton, &QPushButton::clicked, this, &FrpcPanel::createProfile);
    connect(m_copyButton, &QPushButton::clicked, this, &FrpcPanel::duplicateProfile);
    connect(m_renameButton, &QPushButton::clicked, this, &FrpcPanel::renameProfile);
    connect(m_deleteButton, &QPushButton::clicked, this, &FrpcPanel::deleteProfile);

    // ---- 第二行：frpc 可执行文件
    auto *line2 = new QHBoxLayout;
    line2->setSpacing(8);
    line2->addWidget(makeCaption(QStringLiteral("frpc"), bar));

    m_binPath = new QLineEdit(bar);
    m_binPath->setPlaceholderText(QStringLiteral("frpc 可执行文件路径（留空则自动在 PATH / 程序目录里找）"));
    m_binPath->setClearButtonEnabled(true);
    line2->addWidget(m_binPath, 1);

    m_browseButton = makeSmallButton(QStringLiteral("浏览…"));
    m_detectButton = makeSmallButton(QStringLiteral("自动探测"));
    line2->addWidget(m_browseButton);
    line2->addWidget(m_detectButton);
    rows->addLayout(line2);

    connect(m_browseButton, &QPushButton::clicked, this, &FrpcPanel::browseExecutable);
    connect(m_detectButton, &QPushButton::clicked, this, &FrpcPanel::detectExecutable);
    connect(m_binPath, &QLineEdit::textEdited, this, [this] { m_saveTimer->start(); });
    connect(m_binPath, &QLineEdit::editingFinished, this, [this] { saveCurrent(true); });

    return bar;
}

QWidget *FrpcPanel::buildStatusCard()
{
    auto *card = new QWidget(this);
    // 先设 objectName 再 applyStyle：qss 是按 objectName 选择器匹配的，
    // 反过来写样式表在 setObjectName 之前就已经算过一次，可能不生效。
    card->setObjectName(QStringLiteral("frpcCard"));
    // 底色用 @card（深色下比页面亮一档）而不是 @bg-alt：后者在深色主题里比页面还暗，
    // 整块状态卡看起来像挖了个洞，而不是一张卡片。
    xtools::theme::applyStyle(card, QStringLiteral(
        "QWidget#frpcCard{background:@card;border:1px solid @border;border-radius:10px;}"));

    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(14, 10, 14, 10);
    row->setSpacing(12);

    m_statePill = new QLabel(card);
    m_statePill->setAlignment(Qt::AlignCenter);
    row->addWidget(m_statePill);

    m_endpointLabel = new QLabel(card);
    xtools::theme::applyStyle(m_endpointLabel, QStringLiteral("font-size:12px;color:@text-second;"));
    row->addWidget(m_endpointLabel);

    m_uptimeLabel = new QLabel(card);
    xtools::theme::applyStyle(m_uptimeLabel, QStringLiteral("font-size:12px;color:@text-faint;"));
    row->addWidget(m_uptimeLabel);

    row->addStretch(1);

    m_probeButton = new QPushButton(QStringLiteral("探活"), card);
    m_probeButton->setProperty("compact", true);
    m_probeButton->setToolTip(QStringLiteral("用 TCP 连一下配置里的服务端地址，确认端口通不通"));
    row->addWidget(m_probeButton);

    m_runButton = new QPushButton(QStringLiteral("启动"), card);
    m_runButton->setProperty("accent", true);
    m_runButton->setToolTip(QStringLiteral("Ctrl+Enter"));
    row->addWidget(m_runButton);

    connect(m_probeButton, &QPushButton::clicked, this, &FrpcPanel::probeServer);
    connect(m_runButton, &QPushButton::clicked, this, &FrpcPanel::toggleRun);

    return card;
}

QWidget *FrpcPanel::buildConfigArea()
{
    auto *box = new QWidget(this);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    m_formatLabel = makeCaption(QStringLiteral("配置（TOML）"), box);
    head->addWidget(m_formatLabel);
    head->addStretch(1);

    m_validationLabel = new QLabel(box);
    xtools::theme::applyStyle(m_validationLabel, QStringLiteral("font-size:12px;color:@text-faint;"));
    m_validationLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // 这个标签占满剩余宽度（长文本要被裁而不是把左边标题挤走），
    // 所以文字必须显式靠右，否则会飘在整行中间。
    m_validationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    head->addWidget(m_validationLabel, 1);
    column->addLayout(head);

    m_configEdit = new QPlainTextEdit(box);
    m_configEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_configEdit->setTabStopDistance(28);   // 缩进两个等宽字宽，Tab 才不会跨一大截
    styleCodeEditor(m_configEdit);
    column->addWidget(m_configEdit, 1);

    connect(m_configEdit, &QPlainTextEdit::textChanged, this, [this] {
        m_saveTimer->start();
        m_validateTimer->start();
    });

    return box;
}

QWidget *FrpcPanel::buildLogArea()
{
    auto *box = new QWidget(this);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    head->addWidget(makeCaption(QStringLiteral("运行日志"), box));
    head->addStretch(1);

    auto *copyButton = new QPushButton(QStringLiteral("复制"), box);
    copyButton->setProperty("compact", true);
    auto *clearButton = new QPushButton(QStringLiteral("清空"), box);
    clearButton->setProperty("compact", true);
    head->addWidget(copyButton);
    head->addWidget(clearButton);
    column->addLayout(head);

    m_logView = new QTextEdit(box);
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QTextEdit::NoWrap);
    m_logView->setUndoRedoEnabled(false);   // 只读日志不需要撤销栈，省内存
    // 空态给一句话：不然一块空白区域看不出是用来干什么的
    m_logView->setPlaceholderText(QStringLiteral("启动后这里会实时显示 frpc 的输出（含错误原因）"));
    styleLogView(m_logView);
    m_renderedDark = xtools::theme::isDark();
    column->addWidget(m_logView, 1);

    connect(copyButton, &QPushButton::clicked, this, &FrpcPanel::copyLog);
    connect(clearButton, &QPushButton::clicked, this, &FrpcPanel::clearLogView);

    return box;
}

void FrpcPanel::reloadProfileCombo(int selectIndex)
{
    {
        // 重建条目会触发 currentIndexChanged；此时 m_currentIndex 还没更新，
        // 不挡信号会走一遍「保存旧档 + 加载新档」，把内存里的配置写乱。
        const QSignalBlocker blocker(m_profileCombo);
        m_profileCombo->clear();
        for (int i = 0; i < m_profiles.size(); ++i)
            m_profileCombo->addItem(QStringLiteral("%1  ·  %2")
                                        .arg(m_profiles.at(i).name,
                                             frpctext::formatName(frpctext::sniffFormat(
                                                 m_profiles.at(i).config))));
    }

    if (selectIndex < 0 || selectIndex >= m_profiles.size())
        selectIndex = 0;
    // 这里必须能让 currentIndexChanged 生效，否则 loadProfile 不会被调用 ——
    // 所以先设 m_currentIndex = -1，让回调认为"真的换了"。
    m_currentIndex = -1;
    m_profileCombo->setCurrentIndex(selectIndex);
    // 正常情况下上面的信号已经调过 loadProfile 了（会写回 m_currentIndex）；
    // 只有 setCurrentIndex 没触发信号时才需要手动补一次，避免面板停在旧内容上。
    if (m_currentIndex < 0)
        loadProfile(selectIndex);
}

void FrpcPanel::loadProfile(int index)
{
    if (index < 0 || index >= m_profiles.size())
        return;

    m_currentIndex = index;
    const frpc::Profile &profile = m_profiles.at(index);

    {
        // 回填内容不该触发「自动保存」与「校验防抖」：它们会把刚加载的内容当成用户输入，
        // 在 700ms 后用另一份档的内容覆盖存档。
        const QSignalBlocker blockEdit(m_configEdit);
        const QSignalBlocker blockPath(m_binPath);
        m_configEdit->setPlainText(profile.config);
        m_binPath->setText(profile.binPath);
    }

    m_store.setActiveIndex(index);
    refreshValidation();
    refreshState();
}

void FrpcPanel::saveCurrent(bool immediate)
{
    if (m_currentIndex < 0 || m_currentIndex >= m_profiles.size())
        return;

    frpc::Profile &profile = m_profiles[m_currentIndex];
    const QString text = m_configEdit->toPlainText();
    const QString bin = m_binPath->text().trimmed();

    if (profile.config == text && profile.binPath == bin) {
        if (immediate)
            m_saveTimer->stop();
        return;
    }

    profile.config = text;
    profile.binPath = bin;
    m_store.setProfiles(m_profiles);   // 内部会保留 activeIndex
    if (immediate)
        m_saveTimer->stop();
}

frpc::Profile *FrpcPanel::currentProfile()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_profiles.size())
        return nullptr;
    return &m_profiles[m_currentIndex];
}

void FrpcPanel::createProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建配置"),
                                               QStringLiteral("名称"), QLineEdit::Normal,
                                               QStringLiteral("新配置"), &ok)
                             .trimmed();
    if (!ok)
        return;

    saveCurrent(true);

    frpc::Profile profile;
    profile.id = frpc::Store::newId();
    profile.name = name.isEmpty() ? QStringLiteral("未命名配置") : name;
    profile.binPath = m_binPath->text().trimmed();   // 沿用当前填的路径，少填一次
    profile.config = frpctext::defaultConfig();

    m_profiles.append(profile);
    m_store.setProfiles(m_profiles);
    reloadProfileCombo(m_profiles.size() - 1);
    setHint(QStringLiteral("已新建「%1」，改完地址就能启动").arg(profile.name), Color::TextMuted);
}

void FrpcPanel::duplicateProfile()
{
    frpc::Profile *source = currentProfile();
    if (!source)
        return;

    saveCurrent(true);

    frpc::Profile copy = *source;
    copy.id = frpc::Store::newId();   // 新 id：临时配置文件与日志不能和原档共用
    copy.name = QStringLiteral("%1 副本").arg(source->name);

    m_profiles.append(copy);
    m_store.setProfiles(m_profiles);
    reloadProfileCombo(m_profiles.size() - 1);
}

void FrpcPanel::renameProfile()
{
    frpc::Profile *profile = currentProfile();
    if (!profile)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("重命名配置"),
                                               QStringLiteral("名称"), QLineEdit::Normal,
                                               profile->name, &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == profile->name)
        return;

    profile->name = name;
    m_store.setProfiles(m_profiles);

    const QSignalBlocker blocker(m_profileCombo);
    m_profileCombo->setItemText(m_currentIndex, QStringLiteral("%1  ·  %2")
                                                    .arg(name, frpctext::formatName(
                                                                   frpctext::sniffFormat(profile->config))));
}

void FrpcPanel::deleteProfile()
{
    if (m_profiles.size() <= 1) {
        setHint(QStringLiteral("至少要保留一份配置"), Color::WarnText);
        return;
    }
    frpc::Profile *profile = currentProfile();
    if (!profile)
        return;

    auto *service = frpc::Service::instance();
    const bool runningThis = service->runsProfile(profile->id);
    const QString question = runningThis
        ? QStringLiteral("「%1」正在运行，删除会先停止 frpc。确定删除？").arg(profile->name)
        : QStringLiteral("确定删除「%1」？配置内容会一起丢掉。").arg(profile->name);
    if (QMessageBox::question(this, QStringLiteral("删除配置"), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    if (runningThis)
        service->stop();

    // 先清掉编辑防抖：否则 700ms 后的保存会把刚删的档又写回存档。
    m_saveTimer->stop();
    m_profiles.removeAt(m_currentIndex);
    m_store.setProfiles(m_profiles);   // 这时 currentIndex 已经越界，不能先 saveCurrent

    reloadProfileCombo(qMin(m_currentIndex, m_profiles.size() - 1));
    setHint(QStringLiteral("已删除"), Color::TextMuted);
}

void FrpcPanel::browseExecutable()
{
    const QString startDir = m_binPath->text().trimmed().isEmpty()
        ? QFileInfo(frpc::locate()).absolutePath()
        : QFileInfo(m_binPath->text().trimmed()).absolutePath();

#if defined(Q_OS_WIN)
    const QString filter = QStringLiteral("frpc (frpc.exe);;可执行文件 (*.exe);;所有文件 (*)");
#else
    // POSIX 上可执行文件没有固定后缀，只能列全部
    const QString filter = QStringLiteral("所有文件 (*)");
#endif
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 frpc 可执行文件"), startDir, filter);
    if (path.isEmpty())
        return;

    m_binPath->setText(path);
    saveCurrent(true);
    setHint(QStringLiteral("已选择 %1").arg(QFileInfo(path).fileName()), Color::TextMuted);
}

void FrpcPanel::detectExecutable()
{
    const QString found = frpc::locate();
    if (found.isEmpty()) {
        setHint(frpc::locateHint(), Color::DangerText);
        return;
    }

    m_binPath->setText(found);
    saveCurrent(true);
    setHint(QStringLiteral("找到 frpc：%1").arg(found), Color::SuccessText);
}

void FrpcPanel::toggleRun()
{
    auto *service = frpc::Service::instance();
    if (service->isRunning() || service->isStopping())
        stopFrpc();
    else
        startFrpc();
}

void FrpcPanel::startFrpc()
{
    saveCurrent(true);

    const QString text = m_configEdit->toPlainText();
    const frpctext::Format format = frpctext::sniffFormat(text);

    // 先把配置问题拦在启动之前：frpc 的报错是英文 + 行号，"缺少 remotePort" 这种
    // 结论它不会直接给，用户得自己去翻文档。能在这里拦住就别让它启动。
    const frpctext::Validation validation = frpctext::validate(text, format);
    if (validation.hasError) {
        setHint(QStringLiteral("配置有问题：%1").arg(validation.summary()), Color::DangerText);
        return;
    }

    QString program = m_binPath->text().trimmed();
    if (program.isEmpty()) {
        program = frpc::locate();
        if (program.isEmpty()) {
            setHint(frpc::locateHint(), Color::DangerText);
            return;
        }
        m_binPath->setText(program);
        saveCurrent(true);
    }

    const frpc::Profile *profile = currentProfile();
    if (!profile)
        return;

    const frpc::Outcome outcome =
        frpc::Service::instance()->start(profile->id, program, text, format);
    if (!outcome.ok) {
        setHint(outcome.error, Color::DangerText);
        return;
    }

    m_wasRunning = true;
    m_stopRequestedAtMs = 0;
    setHint(QStringLiteral("已启动，连接过程看下面的日志"), Color::SuccessText);
    refreshState();
    pumpLog();
}

void FrpcPanel::stopFrpc()
{
    const frpc::Outcome outcome = frpc::Service::instance()->stop();
    if (!outcome.ok) {
        setHint(outcome.error, Color::WarnText);
        return;
    }

    m_stopRequestedAtMs = QDateTime::currentMSecsSinceEpoch();
    setHint(QStringLiteral("正在停止 frpc…"), Color::WarnText);
    refreshState();
}

void FrpcPanel::onServiceStateChanged()
{
    refreshState();
}

void FrpcPanel::refreshState()
{
    auto *service = frpc::Service::instance();
    const frpc::Service::Status &status = service->status();
    const bool running = service->isRunning();
    const bool stopping = service->isStopping();
    const frpc::Profile *profile = currentProfile();

    // ---- 状态胶囊
    const auto setPill = [this](const QString &text, const QString &pattern) {
        // 文案+样式都没变就直接返回：这个函数每 600ms 被调一次，反复 applyStyle
        // 会不停触发 style 重算与重绘（长日志下能明显感到风扇转起来）。
        if (m_statePill->text() == text && m_statePill->property("frpcPillPattern").toString() == pattern)
            return;
        m_statePill->setText(text);
        m_statePill->setProperty("frpcPillPattern", pattern);
        xtools::theme::applyStyle(m_statePill, pattern);
    };
    const QString pillBase = QStringLiteral("QLabel{border-radius:9px;padding:2px 10px;"
                                            "font-size:12px;font-weight:600;%1}");
    if (stopping) {
        setPill(QStringLiteral("● 停止中"),
                pillBase.arg(QStringLiteral("color:@warn-text;background:@bg-subtle;"
                                            "border:1px solid @border;")));
    } else if (running) {
        const bool other = profile && status.profileId != profile->id;
        setPill(other ? QStringLiteral("● 运行中（其它配置）") : QStringLiteral("● 运行中"),
                pillBase.arg(QStringLiteral("color:@success-text;background:@success-bg;"
                                            "border:1px solid @success-border;")));
    } else {
        // 胶囊底色用 @bg-alt：状态卡是 @card，两者在浅色（白/浅灰）与深色
        // （#23272F / #14161B）下都能拉开一档。
        setPill(QStringLiteral("● 未运行"),
                pillBase.arg(QStringLiteral("color:@text-muted;background:@bg-alt;"
                                            "border:1px solid @border-subtle;")));
    }

    // ---- 服务端地址
    const QString text = m_configEdit->toPlainText();
    const frpctext::Endpoint endpoint = frpctext::endpoint(text, frpctext::sniffFormat(text));
    if (endpoint.valid) {
        m_endpointLabel->setText(QStringLiteral("服务端 %1:%2").arg(endpoint.host).arg(endpoint.port));
        m_probeButton->setEnabled(true);
    } else {
        m_endpointLabel->setText(QStringLiteral("服务端未配置"));
        m_probeButton->setEnabled(false);
    }

    // ---- 运行时长
    if (running && status.startedAtMs > 0) {
        const qint64 seconds =
            (QDateTime::currentMSecsSinceEpoch() - status.startedAtMs) / 1000;
        m_uptimeLabel->setText(QStringLiteral("已运行 %1").arg(frpctext::formatUptime(seconds)));
    } else {
        m_uptimeLabel->clear();
    }

    // ---- 按钮
    if (running || stopping) {
        m_runButton->setText(QStringLiteral("停止"));
        m_runButton->setEnabled(!stopping);
    } else {
        m_runButton->setText(QStringLiteral("启动"));
        m_runButton->setEnabled(true);
    }

    // ---- 「刚启动就退出」的提示。用户主动停止不算异常，靠 m_stopRequestedAtMs 区分。
    if (m_wasRunning && !running && !stopping && m_stopRequestedAtMs == 0
        && !status.lastError.isEmpty()) {
        setHint(status.lastError + QStringLiteral("，原因见下方日志"), Color::DangerText);
    } else if (m_wasRunning && !running && m_stopRequestedAtMs != 0) {
        setHint(QStringLiteral("已停止"), Color::TextMuted);
        m_stopRequestedAtMs = 0;
    }
    m_wasRunning = running || stopping;

    // ---- 主题可能在宿主切换后没有 ThemeChange 事件，这里兜一道
    if (m_renderedDark != xtools::theme::isDark())
        rebuildLogView();
}

void FrpcPanel::refreshValidation()
{
    const QString text = m_configEdit->toPlainText();
    const frpctext::Format format = frpctext::sniffFormat(text);
    m_formatLabel->setText(QStringLiteral("配置（%1）").arg(frpctext::formatName(format)));

    const frpctext::Validation validation = frpctext::validate(text, format);

    QString summary;
    Color token = Color::TextFaint;
    if (validation.hasError) {
        summary = validation.summary();
        token = Color::DangerText;
    } else if (!validation.warnings.isEmpty()) {
        summary = validation.summary();
        token = Color::WarnText;
    } else {
        summary = QStringLiteral("配置看起来没问题");
        token = Color::SuccessText;
    }

    xtools::theme::applyStyle(m_validationLabel,
                              QStringLiteral("font-size:12px;color:@%1;")
                                  .arg(xtools::theme::tokenName(token)));
    m_validationLabel->setText(summary);

    // 工具提示里列全：面板上只放一行，剩下的靠悬停看。
    QStringList details;
    for (const frpctext::Issue &issue : validation.issues)
        details << QStringLiteral("✕ %1").arg(issue.describe());
    for (const frpctext::Issue &issue : validation.warnings)
        details << QStringLiteral("! %1").arg(issue.describe());
    m_validationLabel->setToolTip(details.join(QLatin1Char('\n')));

    refreshState();
}

void FrpcPanel::pumpLog()
{
    auto *service = frpc::Service::instance();
    const frpc::Service::LogChunk chunk = service->since(m_logCursor);

    if (chunk.lostEarlier) {
        // 缓冲区把面板还没取走的行滚掉了：整块重画，避免日志中间出现断层。
        m_logLines.clear();
        m_logView->clear();
    }

    if (!chunk.lines.isEmpty())
        appendLogView(chunk.lines);

    m_logCursor = chunk.next;
}

void FrpcPanel::appendLogView(const QStringList &lines)
{
    if (lines.isEmpty())
        return;

    QScrollBar *bar = m_logView->verticalScrollBar();
    // 贴在底部时才自动跟随；用户往上翻看历史时不要把他拽回去。
    const bool follow = bar->value() >= bar->maximum() - 4;

    QTextCursor cursor(m_logView->document());
    cursor.movePosition(QTextCursor::End);

    for (const QString &line : lines) {
        const frpctext::LogKind kind = frpctext::classifyLogLine(line);
        QTextCharFormat format;
        format.setForeground(xtools::theme::color(logToken(kind)));
        cursor.insertText(line + QLatin1Char('\n'), format);
        m_logLines.append({kind, line});
    }

    if (m_logLines.size() > kMaxViewLines)
        trimLogView();

    if (follow)
        bar->setValue(bar->maximum());
}

void FrpcPanel::trimLogView()
{
    const int excess = m_logLines.size() - kMaxViewLines;
    if (excess <= 0)
        return;

    m_logLines.remove(0, excess);

    // 文档里每行是一个 block，删掉前面 excess 个 block（连带换行）。
    QTextCursor cursor(m_logView->document());
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down, QTextCursor::KeepAnchor, excess);
    cursor.removeSelectedText();
}

void FrpcPanel::rebuildLogView()
{
    m_renderedDark = xtools::theme::isDark();

    const QScrollBar *bar = m_logView->verticalScrollBar();
    const bool follow = bar->value() >= bar->maximum() - 4;

    m_logView->clear();
    QTextCursor cursor(m_logView->document());
    for (const LogEntry &entry : m_logLines) {
        QTextCharFormat format;
        format.setForeground(xtools::theme::color(logToken(entry.kind)));
        cursor.insertText(entry.text + QLatin1Char('\n'), format);
    }

    if (follow)
        m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
}

void FrpcPanel::clearLogView()
{
    m_logView->clear();
    m_logLines.clear();
    frpc::Service::instance()->clearLog();
    m_logCursor = 0;
    setHint(QStringLiteral("日志已清空（frpc 的运行不受影响）"), Color::TextMuted);
}

void FrpcPanel::copyLog()
{
    const QString text = m_logView->toPlainText();
    if (text.isEmpty()) {
        setHint(QStringLiteral("日志是空的"), Color::TextMuted);
        return;
    }
    QGuiApplication::clipboard()->setText(text);
    setHint(QStringLiteral("已复制 %1 行日志").arg(m_logLines.size()), Color::SuccessText);
}

void FrpcPanel::probeServer()
{
    const QString text = m_configEdit->toPlainText();
    const frpctext::Endpoint endpoint = frpctext::endpoint(text, frpctext::sniffFormat(text));
    if (!endpoint.valid) {
        setHint(endpoint.error, Color::WarnText);
        return;
    }

    // 上一次还在连就先掐掉，避免两次结果互相覆盖
    if (m_probeSocket) {
        m_probeSocket->abort();
        m_probeSocket->deleteLater();
        m_probeSocket = nullptr;
    }

    auto *socket = new QTcpSocket(this);
    m_probeSocket = socket;
    setHint(QStringLiteral("正在连接 %1:%2 …").arg(endpoint.host).arg(endpoint.port), Color::TextMuted);

    // 一次性收尾：成功 / 失败 / 超时都可能触发，用属性做幂等标记。
    const auto finish = [this, socket](const QString &message, Color token) {
        if (socket->property("frpcProbeDone").toBool())
            return;
        socket->setProperty("frpcProbeDone", true);
        setHint(message, token);
        socket->abort();
        if (m_probeSocket == socket)
            m_probeSocket = nullptr;
        socket->deleteLater();
    };

    connect(socket, &QTcpSocket::connected, this, [finish, endpoint] {
        finish(QStringLiteral("服务端 %1:%2 可以连上").arg(endpoint.host).arg(endpoint.port),
               Color::SuccessText);
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [finish, endpoint](QAbstractSocket::SocketError) {
                finish(QStringLiteral("连不上 %1:%2：端口没开放，或服务端没起 / 被防火墙挡了")
                           .arg(endpoint.host)
                           .arg(endpoint.port),
                       Color::DangerText);
            });

    // 有些网络环境（丢包）连不上也不报错，一直挂着。2.5 秒还没结果就当我们失败。
    QTimer::singleShot(2500, socket, [finish, endpoint] {
        finish(QStringLiteral("连接 %1:%2 超时").arg(endpoint.host).arg(endpoint.port),
               Color::DangerText);
    });

    socket->connectToHost(endpoint.host, static_cast<quint16>(endpoint.port));
}

void FrpcPanel::setHint(const QString &text, Color token)
{
    xtools::theme::applyStyle(m_hint, QStringLiteral("font-size:12px;color:@%1;")
                                           .arg(xtools::theme::tokenName(token)));
    m_hint->setText(text);
    m_hint->setToolTip(text);
}

void FrpcPanel::setIdleHint()
{
    setHint(QStringLiteral("关闭面板不会停止 frpc；退出工具箱时会自动停止。"), Color::TextFaint);
}

void FrpcPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_tickTimer->start();
    refreshValidation();
    refreshState();
    pumpLog();
    if (m_hint->text().isEmpty())
        setIdleHint();
}

void FrpcPanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // 藏起来就停刷新：内嵌面板被页栈藏住后还在读日志纯属浪费
    m_tickTimer->stop();
    saveCurrent(true);
}

void FrpcPanel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ThemeChange || event->type() == QEvent::PaletteChange)
        rebuildLogView();
}

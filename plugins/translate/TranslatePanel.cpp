#include "TranslatePanel.h"
#include "ThemePalette.h"   // 自绘控件用的颜色令牌

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 单次可翻译的最大字符数。限制它不是为了省流量，而是防止有人贴一篇长文后
/// 切成几十段、把有道演示接口的频率限制瞬间打满，最后整篇都降级。
constexpr int kMaxInputChars = 4000;

/// 单段字符上限：同时低于有道演示接口（实测 800 可用、1200 报 103）、
/// 搜狗（实测 1200 仍可用）与 MyMemory（500 报 403），给各家都留出余量。
constexpr int kChunkChars = 400;

/// 相邻两段之间的最短等待。接口失败多半是限流，串行 + 间隔比并发重试有效得多。
constexpr int kChunkGapMs = 300;

/// 任意两次请求之间的最短间隔（含跨引擎重试）。有道 411 在连发时几乎必然出现，
/// 搜狗 0.1 秒连发也会偶发 status=-1，1 秒间隔实测两家都稳定。
constexpr int kMinRequestGapMs = 1000;

/// 自动翻译防抖：停止输入这么久才发请求，避免边打字边打接口。
constexpr int kAutoDebounceMs = 1100;

/// 单次请求超时。Google gtx 在国内通常直接连不上，最后一个兜底不能让用户干等，
/// 6 秒足够得出「网络不可达」的结论并写出可读原因。
constexpr int kRequestTimeoutMs = 6000;

/// 复制提示的展示时长，超时后还原成带来源信息的成功文案。
constexpr int kCopyHintMs = 2500;

/// 初始状态文案。用函数返回而不是文件级 static QString：避免跨翻译单元的
/// 静态初始化顺序问题，也让中文走 QStringLiteral（与其余插件写法一致）。
inline QString idleHint()
{
    return QStringLiteral("输入文本后点击「翻译」，或使用 Ctrl+Enter");
}

/// 下拉框右侧的小三角，自绘原因是全局 qss 的硬性约定：
/// 为了消掉原生 `QComboBox::drop-down` 在圆角外框上甩出的深色 L 形边框，
/// style.qss 给这个子控件写了规则，副作用是 Qt 连默认箭头也不画了。
/// 进程监控面板用同样的自绘子控件补回箭头（见 ProcMonPanel.cpp），翻译面板有
/// 源语言 / 目标语言 / 引擎三个下拉框，漏补时「自动（推荐）」看起来只是一行
/// 普通文字，用户根本不知道能展开选服务。位置由 TranslatePanel::eventFilter
/// 在 Resize / Show 时贴合到 qss 留出的右侧 18px 箭头区。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("transComboArrow"));
        setFixedSize(10, 8);
        // 点击事件必须穿透给下拉框，否则点三角不会弹出候选列表
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

} // namespace

// ---------------------------------------------------------------- 构造 / 销毁

TranslatePanel::TranslatePanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    updateCount();
    setStatus(idleHint());
}

TranslatePanel::~TranslatePanel()
{
    // 面板会被宿主在切换页面时直接销毁，此时在途 reply 的 finished 可能已经排队。
    // 先断开到 this 的连接再 abort：否则回调里会碰到半析构的面板。
    cancelInFlight();
}

void TranslatePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 宿主换页时会重排焦点链，同步 setFocus 会被覆盖，必须延后一拍
    QTimer::singleShot(0, this, [this] { m_input->setFocus(); });
}

void TranslatePanel::setInputText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;

    // setPlainText 会发 textChanged -> scheduleAutoTranslate（1100 ms 防抖）；
    // 这里直接 startTranslate()，进入工具就是「翻译中…」而不是等一秒才动。
    // startTranslate 内部会停掉防抖定时器，不会重复发请求。
    m_input->setPlainText(trimmed);
    m_output->clear();
    updateCount();
    startTranslate();
}

bool TranslatePanel::eventFilter(QObject *watched, QEvent *event)
{
    // 只处理装了自绘箭头的 QComboBox：把三角摆到 qss 留出的右侧 18px 区域，
    // 并在禁用时跟着变淡。窗口拉伸、字体缩放都会触发 Resize，所以不用手动同步。
    auto *combo = qobject_cast<QComboBox *>(watched);
    if (combo) {
        auto *arrow = combo->findChild<QWidget *>(QStringLiteral("transComboArrow"));
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

void TranslatePanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐，与其余插件面板同一套留白约定
    root->setContentsMargins(28, 0, 28, 20);
    root->setSpacing(10);

    // ---- 顶部工具条：源语言 ⇄ 目标语言 / 自动翻译 / 引擎 / 翻译 ----
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    // 三个下拉框统一补自绘箭头。全局 qss 不画默认三角（见 ComboArrow 注释），
    // 不补的话引擎选择看起来只是一行静态文字，用户找不到切换服务的入口。
    // 箭头是 combo 的子控件，位置由 TranslatePanel::eventFilter 跟着尺寸重摆。
    auto addComboArrow = [this](QComboBox *combo) {
        auto *arrow = new ComboArrow(combo);
        arrow->move(combo->width() - 14, (combo->height() - arrow->height()) / 2);
        combo->installEventFilter(this);
    };

    m_source = new QComboBox(this);
    m_source->addItem(QStringLiteral("自动检测语言"), -1);
    const auto &langs = translate::languages();
    for (int i = 0; i < langs.size(); ++i)
        m_source->addItem(langs.at(i).label, i);
    m_source->setMinimumWidth(140);
    toolbar->addWidget(m_source);
    addComboArrow(m_source);

    m_swap = new QPushButton(QStringLiteral("⇄"), this);
    m_swap->setObjectName(QStringLiteral("transSwap"));
    // 全局 QPushButton 有 6px 14px 的 padding，34px 宽的小按钮必须清零，
    // 否则箭头会被挤没（AGENTS.md 踩坑表里有同一条）
    m_swap->setFixedWidth(34);
    m_swap->setToolTip(QStringLiteral("交换源语言与目标语言，并把译文放回输入框"));
    toolbar->addWidget(m_swap);

    m_target = new QComboBox(this);
    for (int i = 0; i < langs.size(); ++i)
        m_target->addItem(langs.at(i).label, i);
    m_target->setMinimumWidth(110);
    // 目标语言默认英语：最常见的使用场景是「中文（或自动检测到的其他语言）→ 英文」。
    // 用展示名定位而不是硬编码索引，语言表顺序调整后默认值不会悄悄漂到别的语言。
    const int englishIndex = m_target->findText(QStringLiteral("英语"));
    m_target->setCurrentIndex(englishIndex >= 0 ? englishIndex : 0);
    toolbar->addWidget(m_target);
    addComboArrow(m_target);

    toolbar->addStretch(1);

    m_autoCheck = new QCheckBox(QStringLiteral("输入后自动翻译"), this);
    // 默认开启，减少常见短文本场景的手动点击；1100ms 防抖与请求间隔限制仍然保留，
    // 避免公开演示接口在连续输入时触发限流。此处先于信号连接，空面板不会因此发请求。
    m_autoCheck->setChecked(true);
    m_autoCheck->setToolTip(QStringLiteral("默认开启：停止输入约 1 秒后自动翻译；接口有频率限制，长文本建议手动翻译"));
    toolbar->addWidget(m_autoCheck);

    // 明确写「引擎」两个字：只放一个「自动（推荐）」下拉框时，用户会把它当成
    // 某种模式开关，而不是可选服务商的入口。
    auto *engineLabel = new QLabel(QStringLiteral("引擎"), this);
    engineLabel->setObjectName(QStringLiteral("transEngineLabel"));
    toolbar->addWidget(engineLabel);

    m_engineCombo = new QComboBox(this);
    m_engineCombo->setObjectName(QStringLiteral("transEngineCombo"));
    // 当前项直接带上家数，配合下方常驻的服务链说明，不用展开也能看出接了多家。
    m_engineCombo->addItem(QStringLiteral("自动（4 家）"), -1);
    // 每项的悬停说明：展开列表里只有服务名，补充各自定位能避免误选
    // （例如 Google 在国内通常不可达，只是海外兜底）。
    const QStringList engineTips = {
        QStringLiteral("依次尝试 有道 → 搜狗 → MyMemory → Google，任一失败整篇换下一家"),
        QStringLiteral("有道公开演示接口，国内速度与质量最好，无需密钥"),
        QStringLiteral("搜狗翻译网页接口，首次使用会先建立会话"),
        QStringLiteral("MyMemory 免费公共接口，单次最多 500 字符"),
        QStringLiteral("Google gtx，国内网络通常不可达，仅作海外兜底")
    };
    m_engineCombo->setItemData(0, engineTips.value(0), Qt::ToolTipRole);
    for (int i = 0; i < static_cast<int>(translate::Engine::Count); ++i) {
        const auto engine = static_cast<translate::Engine>(i);
        m_engineCombo->addItem(translate::engineName(engine), i);
        m_engineCombo->setItemData(i + 1, engineTips.value(i + 1), Qt::ToolTipRole);
    }
    m_engineCombo->setMinimumWidth(120);
    m_engineCombo->setToolTip(QStringLiteral(
        "共 4 家服务：自动模式依次尝试 有道 → 搜狗 → MyMemory → Google，失败会自动降级；"
        "也可锁定其中一家"));
    toolbar->addWidget(m_engineCombo);
    addComboArrow(m_engineCombo);

    m_translateBtn = new QPushButton(QStringLiteral("翻译"), this);
    m_translateBtn->setProperty("accent", true);
    toolbar->addWidget(m_translateBtn);

    root->addLayout(toolbar);

    // 把「接了哪几家」常驻在界面上。只靠一个折叠的下拉框，用户很难发现自动模式
    // 背后有四家服务，甚至会以为只有一个引擎；这行是静态说明，实际使用哪家仍由
    // 顶部「引擎」下拉决定，翻译成功后状态栏还会再报一次真实来源。
    auto *engineHint = new QLabel(
        QStringLiteral("服务：有道 → 搜狗 → MyMemory → Google（自动依次降级，顶部「引擎」可锁定某一家）"),
        this);
    engineHint->setObjectName(QStringLiteral("transEngineHint"));
    // 窄窗口下宁可把说明裁掉，也不要把整行文字宽度变成窗口的最小宽度下限。
    engineHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    root->addWidget(engineHint);

    // ---- 中部双栏：左输入、右译文。两侧等宽，参考有道文本翻译页 ----
    auto *panes = new QHBoxLayout;
    panes->setSpacing(12);

    m_input = new QPlainTextEdit(this);
    m_input->setObjectName(QStringLiteral("transPane"));
    m_input->setPlaceholderText(QStringLiteral("输入要翻译的文本，Ctrl+Enter 翻译"));
    m_input->setMinimumHeight(180);
    panes->addWidget(m_input, 1);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("transPane"));
    m_output->setPlaceholderText(QStringLiteral("译文将显示在这里"));
    m_output->setReadOnly(true);
    m_output->setMinimumHeight(180);
    panes->addWidget(m_output, 1);

    root->addLayout(panes, 1);

    // ---- 底部状态条 ----
    auto *footer = new QHBoxLayout;
    footer->setSpacing(8);

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("transCount"));
    footer->addWidget(m_count);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("transStatus"));
    footer->addWidget(m_status, 1);

    m_copyBtn = new QPushButton(QStringLiteral("复制译文"), this);
    m_copyBtn->setEnabled(false);
    footer->addWidget(m_copyBtn);

    m_clearBtn = new QPushButton(QStringLiteral("清空"), this);
    footer->addWidget(m_clearBtn);

    root->addLayout(footer);

    // ---- 信号集中连接：放在 addItem 之后，避免构造期间的下标变化触发自动翻译 ----
    connect(m_translateBtn, &QPushButton::clicked, this, [this] { startTranslate(); });
    connect(m_swap, &QPushButton::clicked, this, [this] { swapLanguages(); });
    connect(m_copyBtn, &QPushButton::clicked, this, [this] { copyResult(); });
    connect(m_clearBtn, &QPushButton::clicked, this, [this] { clearAll(); });
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] {
        updateCount();
        scheduleAutoTranslate();
    });

    // 两个组合键都注册：不同键盘/Qt 版本对 Ctrl+Enter 与 Ctrl+Return 的映射不一致
    for (const QString &seq : {QStringLiteral("Ctrl+Return"), QStringLiteral("Ctrl+Enter")}) {
        auto *shortcut = new QShortcut(QKeySequence(seq), this);
        // 只在本面板及子控件里生效，不跟主窗口 / 独立窗口的其他快捷键抢按键
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this] { startTranslate(); });
    }

    m_autoTimer = new QTimer(this);
    m_autoTimer->setSingleShot(true);
    connect(m_autoTimer, &QTimer::timeout, this, [this] {
        if (!m_busy && !m_input->toPlainText().trimmed().isEmpty())
            startTranslate();
    });

    connect(m_autoCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            scheduleAutoTranslate();
        else
            m_autoTimer->stop();
    });

    // 改语言方向 / 引擎后，开着自动翻译时立刻重翻一次，避免译文与当前选项不符
    auto onOptionChanged = [this] { scheduleAutoTranslate(); };
    connect(m_source, &QComboBox::currentIndexChanged, this, onOptionChanged);
    connect(m_target, &QComboBox::currentIndexChanged, this, onOptionChanged);
    connect(m_engineCombo, &QComboBox::currentIndexChanged, this, onOptionChanged);
}

int TranslatePanel::sourceIndex() const
{
    return m_source->currentData().toInt();
}

int TranslatePanel::targetIndex() const
{
    return m_target->currentData().toInt();
}

int TranslatePanel::engineChoice() const
{
    return m_engineCombo->currentData().toInt();
}

void TranslatePanel::updateCount()
{
    const int n = m_input->toPlainText().size();
    m_count->setText(QStringLiteral("%1 / %2 字").arg(n).arg(kMaxInputChars));
}

/// 状态文案的三态（idle / busy / error）走动态属性而不是 setStyleSheet：
/// 主题色集中在全局 qss，插件里不写死颜色。改属性后要 unpolish/polish 才会重算样式。
void TranslatePanel::setStatus(const QString &text, const QString &state)
{
    ++m_statusGeneration;   // 让尚未触发的「还原旧提示」定时器自动失效
    m_status->setText(text);
    m_status->setProperty("state", state);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

void TranslatePanel::setBusy(bool busy)
{
    m_busy = busy;
    m_translateBtn->setEnabled(!busy);
    m_translateBtn->setText(busy ? QStringLiteral("翻译中…") : QStringLiteral("翻译"));
    m_swap->setEnabled(!busy);
    m_copyBtn->setEnabled(!busy && !m_output->toPlainText().isEmpty());
}

// ---------------------------------------------------------------- 翻译调度

void TranslatePanel::startTranslate()
{
    if (m_busy)
        return;   // 按钮与自动翻译都已挡住重入，这里是最后一道保险

    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) {
        // 自动翻译的输入被清空时会走到这里，静默回到初始态，不要弹红字吓人
        setStatus(idleHint());
        return;
    }
    if (text.size() > kMaxInputChars) {
        m_output->clear();
        setStatus(QStringLiteral("文本超过 %1 字，请分段翻译").arg(kMaxInputChars),
                  QStringLiteral("error"));
        return;
    }

    m_autoTimer->stop();
    cancelInFlight();

    m_failures.clear();
    m_successStatus.clear();
    m_chunks = translate::splitText(text, kChunkChars);
    m_detectedIndex = translate::detectSource(text);

    m_engineOrder.clear();
    const int choice = engineChoice();
    if (choice < 0) {
        for (int i = 0; i < static_cast<int>(translate::Engine::Count); ++i)
            m_engineOrder.append(static_cast<translate::Engine>(i));
    } else {
        // 用户显式选了某个服务：只试它，失败就如实报错，不擅自换别家
        m_engineOrder.append(static_cast<translate::Engine>(choice));
    }
    m_enginePos = 0;
    m_engine = m_engineOrder.first();
    m_engineUsed.clear();
    m_lastRequestMs = 0;

    m_output->clear();
    m_copyBtn->setEnabled(false);
    setBusy(true);
    setStatus(QStringLiteral("正在翻译…（共 %1 段）").arg(m_chunks.size()),
              QStringLiteral("busy"));
    beginEngine();
}

void TranslatePanel::beginEngine()
{
    // 换引擎时整篇重译：让一次结果的译文来自同一个服务，混着两家口径很难解释，
    // 也避免把上一个引擎已成功的前几段和新引擎的后几段拼成一篇。
    m_chunkIndex = 0;
    m_translations.clear();

    // 搜狗必须先 GET 首页建立会话（cookie + secretCode），否则接口固定返回 s10。
    // 会话参数只放在面板实例里：宿主不缓存面板，离开即失效，也避免两份面板串会话。
    if (m_engine == translate::Engine::Sogou && m_sogouSecret.isEmpty()) {
        startSogouHandshake();
        return;
    }
    requestCurrentChunk();
}

void TranslatePanel::startSogouHandshake()
{
    QNetworkRequest request(translate::sogouHandshakeUrl());
    request.setTransferTimeout(kRequestTimeoutMs);
    // 与后续翻译请求共用同一个浏览器 UA（详见 TranslateLogic.h::sogouUserAgent）。
    request.setRawHeader("User-Agent", translate::sogouUserAgent());

    m_lastRequestMs = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = m_nam.get(request);
    m_reply = reply;

    const int generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        onSogouHandshakeFinished(generation, reply);
    });
    setStatus(QStringLiteral("正在建立搜狗会话…"), QStringLiteral("busy"));
}

void TranslatePanel::onSogouHandshakeFinished(int generation, QNetworkReply *reply)
{
    if (generation != m_generation) {
        reply->deleteLater();
        return;
    }
    m_reply.clear();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        degradeOrFail(reply->errorString());
        return;
    }
    const auto session = translate::parseSogouHandshake(reply->readAll());
    if (!session.ok) {
        degradeOrFail(session.error);
        return;
    }
    m_sogouSecret = session.secretCode;
    m_sogouUuid = session.uuid;
    // 握手也算一次请求，走同一个节流窗口后再发第一段，避免紧接着撞上限流。
    requestCurrentChunk();
}

void TranslatePanel::requestCurrentChunk()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 wait = 0;
    if (m_lastRequestMs > 0) {
        // 两个约束取较严者：段间至少 300ms，任意两次请求至少 1000ms
        wait = qMax<qint64>(kChunkGapMs,
                            kMinRequestGapMs - (now - m_lastRequestMs));
    }
    const int generation = m_generation;
    if (wait > 0) {
        QTimer::singleShot(static_cast<int>(wait), this, [this, generation] {
            if (generation == m_generation)
                sendCurrentChunk();
        });
    } else {
        sendCurrentChunk();
    }
}

void TranslatePanel::sendCurrentChunk()
{
    // 空片段来自原文的空行：不值得为它发请求，但要在译文列表里占位，
    // 这样 joinChunks() 才能把换行还回去。
    while (m_chunkIndex < m_chunks.size()
           && m_chunks.at(m_chunkIndex).text.trimmed().isEmpty()) {
        m_translations.append(QString());
        ++m_chunkIndex;
    }
    if (m_chunkIndex >= m_chunks.size()) {
        finishSuccess();
        return;
    }

    const QString piece = m_chunks.at(m_chunkIndex).text;
    auto params = translate::makeParams(piece, sourceIndex(), targetIndex(),
                                        m_detectedIndex);
    // 搜狗的会话参数由握手阶段填进面板，纯逻辑层不缓存任何全局状态。
    params.sogouSecret = m_sogouSecret;
    params.sogouUuid = m_sogouUuid;
    const auto spec = translate::buildRequest(m_engine, params);

    // buildRequest 能在不发请求的情况下判定「这家服务不支持当前方向」。
    // 直接走降级，别把搜狗原样返回的原文或反了方向的译文当成结果展示。
    if (!spec.error.isEmpty()) {
        degradeOrFail(spec.error);
        return;
    }
    if (spec.url.isEmpty()) {
        degradeOrFail(QStringLiteral("无法构造请求"));
        return;
    }

    QNetworkRequest request(spec.url);
    request.setTransferTimeout(kRequestTimeoutMs);
    // 少数公开接口会拒绝没有 UA 的请求；搜狗必须用浏览器 UA，其余沿用默认标识。
    request.setRawHeader("User-Agent", spec.userAgent.isEmpty()
                                           ? QByteArray("cxk-tools/translate")
                                           : spec.userAgent);
    if (!spec.referer.isEmpty())
        request.setRawHeader("Referer", spec.referer);
    if (spec.isPost) {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QByteArray("application/x-www-form-urlencoded"));
    }

    m_lastRequestMs = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply *reply = spec.isPost ? m_nam.post(request, spec.body)
                                       : m_nam.get(request);
    m_reply = reply;

    const int generation = m_generation;
    const translate::Engine engine = m_engine;
    // 以 this 为上下文：面板被销毁后连接自动断开，不会回调到已释放的对象
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, engine] {
                onReplyFinished(generation, engine, reply);
            });

    if (m_chunks.size() > 1) {
        setStatus(QStringLiteral("正在翻译第 %1 / %2 段（%3）…")
                      .arg(m_chunkIndex + 1)
                      .arg(m_chunks.size())
                      .arg(translate::engineName(engine)),
                  QStringLiteral("busy"));
    }
}

void TranslatePanel::onReplyFinished(int generation, translate::Engine engine,
                                     QNetworkReply *reply)
{
    // 用户重新点了翻译 / 点了清空 / 面板已销毁：旧回复整包丢弃
    if (generation != m_generation) {
        reply->deleteLater();
        return;
    }
    m_reply.clear();
    reply->deleteLater();

    QString reason;
    if (reply->error() != QNetworkReply::NoError) {
        reason = reply->errorString();
    } else {
        const auto parsed = translate::parseResponse(engine, reply->readAll());
        if (!parsed.ok) {
            reason = parsed.error;
            // 搜狗 s10 = cookie/签名已失效：清掉缓存，下次轮到它时重新握手。
            if (parsed.sessionExpired)
                m_sogouSecret.clear();
        } else {
            m_translations.append(parsed.text);
            ++m_chunkIndex;
            if (m_chunkIndex < m_chunks.size()) {
                requestCurrentChunk();
            } else {
                m_engineUsed = translate::engineName(engine);
                finishSuccess();
            }
            return;
        }
    }

    degradeOrFail(reason.isEmpty() ? QStringLiteral("服务没有返回内容") : reason);
}

void TranslatePanel::degradeOrFail(const QString &reason)
{
    m_failures.append(QStringLiteral("%1：%2")
                          .arg(translate::engineName(m_engine), reason));

    if (m_enginePos + 1 < m_engineOrder.size()) {
        ++m_enginePos;
        m_engine = m_engineOrder.at(m_enginePos);
        setStatus(QStringLiteral("%1 不可用（%2），正在改用 %3…")
                      .arg(translate::engineName(m_engineOrder.at(m_enginePos - 1)),
                           reason, translate::engineName(m_engine)),
                  QStringLiteral("busy"));
        beginEngine();
        return;
    }
    finishFailure();
}

void TranslatePanel::finishSuccess()
{
    const QString result = translate::joinChunks(m_chunks, m_translations);
    m_output->setPlainText(result);
    setBusy(false);
    m_copyBtn->setEnabled(!result.isEmpty());

    const QString detected = translate::languages().at(m_detectedIndex).label;
    QString msg = QStringLiteral("已由「%1」翻译 · 检测到：%2")
                      .arg(m_engineUsed, detected);
    if (m_chunks.size() > 1)
        msg += QStringLiteral(" · 分 %1 段").arg(m_chunks.size());
    m_successStatus = msg;
    setStatus(msg);
}

void TranslatePanel::finishFailure()
{
    setBusy(false);
    m_output->clear();
    m_copyBtn->setEnabled(false);
    // 把所有尝试过的原因都列出来：AGENTS.md 规则 3 要求失败必须有可读原因，
    // 不允许只写「翻译失败」让用户不知道是断网、限流还是语言方向不支持。
    const QString reasons = m_failures.join(QStringLiteral("；"));
    setStatus(QStringLiteral("翻译失败：%1。请检查网络后重试").arg(reasons),
              QStringLiteral("error"));
}

void TranslatePanel::cancelInFlight()
{
    ++m_generation;   // 让所有在途回复与延迟启动的定时器失效
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
}

// ---------------------------------------------------------------- 顺手操作

void TranslatePanel::swapLanguages()
{
    if (m_busy)
        return;

    const int src = sourceIndex();
    const int dst = targetIndex();
    // 自动检测模式下用本地推断出的语言参与交换，否则「自动 ⇄ 中文」没有意义
    const int effectiveSrc = (src >= 0) ? src : m_detectedIndex;
    if (effectiveSrc == dst) {
        setStatus(QStringLiteral("源语言与目标语言相同，无需交换"));
        return;
    }

    const QString translated = m_output->toPlainText().trimmed();
    {
        // 改输入会触发 textChanged → 自动翻译，交换动作里要先挡掉这一下
        QSignalBlocker blocker(m_input);
        if (!translated.isEmpty())
            m_input->setPlainText(translated);
    }
    {
        QSignalBlocker b1(m_source);
        QSignalBlocker b2(m_target);
        m_source->setCurrentIndex(m_source->findData(dst));
        m_target->setCurrentIndex(m_target->findData(effectiveSrc));
    }
    m_output->clear();
    m_copyBtn->setEnabled(false);
    m_detectedIndex = translate::detectSource(m_input->toPlainText());
    updateCount();
    setStatus(translated.isEmpty()
                  ? QStringLiteral("已交换语言方向")
                  : QStringLiteral("已交换语言方向，译文已放回输入框"));
    scheduleAutoTranslate();
}

void TranslatePanel::copyResult()
{
    const QString result = m_output->toPlainText();
    if (result.isEmpty()) {
        setStatus(QStringLiteral("还没有译文可复制"));
        return;
    }
    QGuiApplication::clipboard()->setText(result);
    setStatus(QStringLiteral("译文已复制到剪贴板"));

    // 提示 2.5 秒后还原成带来源信息的文案；期间用户又做了别的操作时，
    // setStatus() 会递增 m_statusGeneration，旧定时器就不会把新提示抹掉。
    const int generation = m_statusGeneration;
    const QString restore = m_successStatus;
    QTimer::singleShot(kCopyHintMs, this, [this, generation, restore] {
        if (generation == m_statusGeneration && !restore.isEmpty())
            setStatus(restore);
    });
}

void TranslatePanel::clearAll()
{
    cancelInFlight();
    setBusy(false);
    {
        QSignalBlocker blocker(m_input);
        m_input->clear();
    }
    m_output->clear();
    m_copyBtn->setEnabled(false);
    m_successStatus.clear();
    m_autoTimer->stop();
    updateCount();
    setStatus(idleHint());
    m_input->setFocus();
}

void TranslatePanel::scheduleAutoTranslate()
{
    if (!m_autoCheck || !m_autoCheck->isChecked() || m_busy)
        return;
    if (m_input->toPlainText().trimmed().isEmpty()) {
        m_autoTimer->stop();
        return;
    }
    m_autoTimer->start(kAutoDebounceMs);
}

#include "ScreenRecPanel.h"

#include "RegionSelectOverlay.h"
#include "ScreenGrabber.h"
#include "ThemePalette.h"
#include "LayoutMetrics.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtDebug>

using namespace screenrec;
namespace theme = xtools::theme;

namespace {

/// 下拉框右侧的小三角。
///
/// 全局 qss 给 `QComboBox::drop-down` 去掉了原生那条甩到圆角框外的深色 L 形边框，
/// 代价是 Qt 同时也不画默认箭头了；用 `::down-arrow { image }` 会把图片重复画到
/// 文字后面，改 `background-image` 又整片平铺。自绘一个透明子控件最省事
/// （与 plugins/procmon/ProcMonPanel.cpp 的做法一致）。
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
        // 自绘元素跟主题走：写死浅灰在深色底上几乎看不见
        painter.setBrush(isEnabled() ? theme::color(theme::Color::TextFaint)
                                     : theme::color(theme::Color::TextDisabled));
        QPainterPath path;
        path.moveTo(0.5, 2.0);
        path.lineTo(9.5, 2.0);
        path.lineTo(5.0, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
    }
};

/// 状态行/说明文字的统一出口。模板里带 @令牌，换肤时宿主遍历所有控件重算。
void applyText(QLabel *label, const QString &text, theme::Color token, int pixelSize = 12)
{
    theme::applyStyle(label, QStringLiteral("font-size:%1px;color:@%2;")
                                 .arg(pixelSize)
                                 .arg(theme::tokenName(token)));
    label->setText(text);
}

/// 小标题（"屏幕""帧率"这类字段名）。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    theme::applyStyle(label, QStringLiteral("font-size:12px;color:@text-muted;"));
    return label;
}

/// 改动态属性后必须重新 polish，否则 qss 里 [accent="true"] 这类规则不会立刻生效。
void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString formatDuration(qint64 ms)
{
    const qint64 totalSeconds = qMax<qint64>(0, ms) / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QString formatBytes(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("0 B");
    constexpr qint64 mb = 1024 * 1024;
    if (bytes >= mb)
        return QStringLiteral("%1 MB").arg(double(bytes) / mb, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 0);
}

} // namespace

ScreenRecPanel::ScreenRecPanel(QWidget *parent)
    : QWidget(parent),
      m_service(RecordService::instance())
{
    m_outputDir = RecordService::defaultOutputDir();
    m_hasFfmpeg = !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();

    auto *root = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐，视觉上像同一栏
    root->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    root->setSpacing(12);

    root->addWidget(buildSetupCard());
    root->addWidget(buildLiveCard());

    // ---- 操作行
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);

    m_startStop = new QPushButton(QStringLiteral("开始录制"), this);
    m_startStop->setObjectName(QStringLiteral("recStartStop"));
    m_startStop->setCursor(Qt::PointingHandCursor);
    m_startStop->setProperty("accent", true);
    theme::applyStyle(m_startStop,
                      QStringLiteral("QPushButton{min-height:38px;font-size:14px;padding:0 22px;}"));
    connect(m_startStop, &QPushButton::clicked, this, &ScreenRecPanel::onStartStop);
    actions->addWidget(m_startStop);

    m_convert = new QPushButton(QStringLiteral("转成 MP4"), this);
    m_convert->setObjectName(QStringLiteral("recConvert"));
    m_convert->setCursor(Qt::PointingHandCursor);
    m_convert->setVisible(false);   // 录完且装了 ffmpeg 才出现
    connect(m_convert, &QPushButton::clicked, this, &ScreenRecPanel::onConvert);
    actions->addWidget(m_convert);

    m_openFile = new QPushButton(QStringLiteral("打开视频"), this);
    m_openFile->setObjectName(QStringLiteral("recOpenFile"));
    m_openFile->setCursor(Qt::PointingHandCursor);
    m_openFile->setVisible(false);
    connect(m_openFile, &QPushButton::clicked, this, &ScreenRecPanel::onOpenFile);
    actions->addWidget(m_openFile);

    actions->addStretch(1);
    root->addLayout(actions);

    // ---- 底部状态行
    auto *footer = new QHBoxLayout;
    footer->setSpacing(12);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("recStatus"));
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);

    m_hint = new QLabel(QStringLiteral("切到其他页面不会中断录制 · 不含声音"), this);
    m_hint->setObjectName(QStringLiteral("recHint"));
    m_hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    theme::applyStyle(m_hint, QStringLiteral("font-size:11px;color:@text-faint;"));
    footer->addWidget(m_hint, 0);

    root->addLayout(footer);

    // 多余的垂直空间留白：不加这句，QVBoxLayout 会把实时卡片撑满整页
    // （卡片里有个 addStretch，会让它的 sizeHint 变得很高），中间出现一大块空白。
    root->addStretch(1);

    // ---- 订阅录制器。以 this 为 context：面板被销毁时自动断开，录制继续。
    connect(m_service, &RecordService::stateChanged, this, &ScreenRecPanel::onStateChanged);
    connect(m_service, &RecordService::finished, this, &ScreenRecPanel::onFinished);
    connect(m_service, &RecordService::failed, this, &ScreenRecPanel::onFailed);
    connect(m_service, &RecordService::cancelled, this, [this] {
        setStatus(QStringLiteral("已取消"), theme::Color::TextFaint);
    });

    m_liveTimer = new QTimer(this);
    m_liveTimer->setInterval(200);
    connect(m_liveTimer, &QTimer::timeout, this, &ScreenRecPanel::refreshLive);

    // 恢复录制器的当前状态：用户可能在"录制中"离开本页，回来要接着显示。
    // applyState 是幂等只读的，所以这里不需要任何"是不是第一次"的判断。
    applyState(m_service->state());
    refreshLive();
}

ScreenRecPanel::~ScreenRecPanel()
{
    // 刻意什么都不做。
    // 面板被销毁（回首页 / 切工具 / 关窗口进托盘）时不能停录制 —— 录屏工具被
    // "点一下首页就断掉"是不可接受的。录制器是模块级的，见 RecordService.h。
    // m_liveTimer 是 this 的子对象，会自动停掉；m_regionOverlay 是独立顶层窗口，
    // 用 QPointer 兜住，不在这里删（用户可能正拖到一半）。
}

QWidget *ScreenRecPanel::buildSetupCard()
{
    m_setupCard = new QFrame(this);
    m_setupCard->setObjectName(QStringLiteral("recSetupCard"));
    // QWidget/QFrame 子类必须开 WA_StyledBackground 才会画 qss 背景
    m_setupCard->setAttribute(Qt::WA_StyledBackground, true);
    theme::applyStyle(m_setupCard, QStringLiteral(
        "QFrame#recSetupCard{background:@bg;border:1px solid @border;border-radius:10px;}"));

    auto *outer = new QVBoxLayout(m_setupCard);
    outer->setContentsMargins(18, 14, 18, 14);
    outer->setSpacing(10);

    // ---- 第一行：屏幕 / 帧率 / 画质
    auto *row1 = new QHBoxLayout;
    row1->setSpacing(8);
    row1->addWidget(makeCaption(QStringLiteral("屏幕"), m_setupCard));
    m_screenBox = new QComboBox(m_setupCard);
    m_screenBox->setObjectName(QStringLiteral("recScreen"));
    // 下拉框不要撑满整行：三组参数靠左排布更紧凑，也避免"屏幕"一栏被拉到
    // 半屏宽、右边两组被挤到最右侧。
    m_screenBox->setMinimumWidth(190);
    m_screenBox->setMaximumWidth(280);
    m_screenBox->installEventFilter(this);
    m_arrows.insert(m_screenBox, new ComboArrow(m_screenBox));
    connect(m_screenBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        // 换了屏幕，之前框选的区域就不再成立（区域坐标是相对某一块屏的）
        m_regionLogic = QRect();
        m_regionScreen.clear();
        applyDefaultFpsForScreen();
        refreshRegionInfo();
    });
    row1->addWidget(m_screenBox);

    row1->addSpacing(12);

    row1->addSpacing(8);
    row1->addWidget(makeCaption(QStringLiteral("帧率"), m_setupCard));
    m_fpsBox = new QComboBox(m_setupCard);
    m_fpsBox->setObjectName(QStringLiteral("recFps"));
    m_fpsBox->setFixedWidth(96);
    m_fpsBox->installEventFilter(this);
    m_arrows.insert(m_fpsBox, new ComboArrow(m_fpsBox));
    // 帧率越高越流畅，但抓屏+编码是实打实的开销：15fps 在 1080p 上很稳，
    // 30fps 只建议给小块区域用（文案里已经写了"可能掉帧"）。
    m_fpsBox->addItem(QStringLiteral("10 fps"), 10);
    m_fpsBox->addItem(QStringLiteral("15 fps"), 15);
    m_fpsBox->addItem(QStringLiteral("24 fps"), 24);
    m_fpsBox->addItem(QStringLiteral("30 fps"), 30);
    row1->addWidget(m_fpsBox);

    row1->addSpacing(8);
    row1->addWidget(makeCaption(QStringLiteral("画质"), m_setupCard));
    m_qualityBox = new QComboBox(m_setupCard);
    m_qualityBox->setObjectName(QStringLiteral("recQuality"));
    m_qualityBox->setFixedWidth(104);
    m_qualityBox->installEventFilter(this);
    m_arrows.insert(m_qualityBox, new ComboArrow(m_qualityBox));
    m_qualityBox->addItem(QStringLiteral("流畅"), 60);
    m_qualityBox->addItem(QStringLiteral("标准"), 75);
    m_qualityBox->addItem(QStringLiteral("高清"), 85);
    m_qualityBox->setCurrentIndex(1);   // 标准 q75 是体积与清晰度的拐点
    row1->addWidget(m_qualityBox);
    // 留白放行末，让三组参数靠左紧凑排列（放中间会把它们推到右边缘）
    row1->addStretch(1);
    outer->addLayout(row1);

    // ---- 第二行：范围
    auto *row2 = new QHBoxLayout;
    row2->setSpacing(8);
    row2->addWidget(makeCaption(QStringLiteral("范围"), m_setupCard));

    m_modeBox = new QComboBox(m_setupCard);
    m_modeBox->setObjectName(QStringLiteral("recMode"));
    m_modeBox->setFixedWidth(150);
    m_modeBox->installEventFilter(this);
    m_arrows.insert(m_modeBox, new ComboArrow(m_modeBox));
    m_modeBox->addItem(QStringLiteral("整屏"), 0);
    m_modeBox->addItem(QStringLiteral("自定义区域"), 1);
    connect(m_modeBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        refreshRegionInfo();
    });
    row2->addWidget(m_modeBox);

    m_pickRegion = new QPushButton(QStringLiteral("框选区域…"), m_setupCard);
    m_pickRegion->setObjectName(QStringLiteral("recPickRegion"));
    m_pickRegion->setCursor(Qt::PointingHandCursor);
    // 窄按钮必须清掉全局 padding，否则文字会被挤成一个点
    theme::applyStyle(m_pickRegion,
                      QStringLiteral("QPushButton{padding:0 12px;min-height:26px;font-size:12px;}"));
    connect(m_pickRegion, &QPushButton::clicked, this, &ScreenRecPanel::onPickRegion);
    row2->addWidget(m_pickRegion);

    m_regionInfo = new QLabel(m_setupCard);
    m_regionInfo->setObjectName(QStringLiteral("recRegionInfo"));
    theme::applyStyle(m_regionInfo, QStringLiteral("font-size:12px;color:@text-faint;"));
    row2->addWidget(m_regionInfo, 1);
    outer->addLayout(row2);

    // ---- 第三行：光标 / 输出目录
    auto *row3 = new QHBoxLayout;
    row3->setSpacing(8);

    m_cursorBox = new QCheckBox(QStringLiteral("录制鼠标光标"), m_setupCard);
    m_cursorBox->setObjectName(QStringLiteral("recCursor"));
    m_cursorBox->setChecked(true);
    row3->addWidget(m_cursorBox);
    row3->addStretch(1);

    row3->addWidget(makeCaption(QStringLiteral("输出到"), m_setupCard));
    m_outDirLabel = new QLabel(m_setupCard);
    m_outDirLabel->setObjectName(QStringLiteral("recOutDir"));
    theme::applyStyle(m_outDirLabel, QStringLiteral("font-size:12px;color:@text-body;"));
    // 只显示最后两级目录（"视频/cxk-tools"），完整路径放 tooltip：
    // 长路径会把这一行撑爆，而用户其实只需要知道"大概在哪儿"。
    {
        const QDir dir(m_outputDir);
        const QString leaf = dir.dirName();
        const QString parent = QFileInfo(m_outputDir).dir().dirName();
        m_outDirLabel->setText(parent.isEmpty() ? leaf : parent + QStringLiteral("/") + leaf);
        m_outDirLabel->setToolTip(m_outputDir);
    }
    row3->addWidget(m_outDirLabel);

    m_chooseDir = new QPushButton(QStringLiteral("更改…"), m_setupCard);
    m_chooseDir->setObjectName(QStringLiteral("recChooseDir"));
    m_chooseDir->setCursor(Qt::PointingHandCursor);
    theme::applyStyle(m_chooseDir,
                      QStringLiteral("QPushButton{padding:0 12px;min-height:26px;font-size:12px;}"));
    connect(m_chooseDir, &QPushButton::clicked, this, &ScreenRecPanel::onChooseDir);
    row3->addWidget(m_chooseDir);

    m_openDir = new QPushButton(QStringLiteral("打开目录"), m_setupCard);
    m_openDir->setObjectName(QStringLiteral("recOpenDir"));
    m_openDir->setCursor(Qt::PointingHandCursor);
    theme::applyStyle(m_openDir,
                      QStringLiteral("QPushButton{padding:0 12px;min-height:26px;font-size:12px;}"));
    connect(m_openDir, &QPushButton::clicked, this, &ScreenRecPanel::onOpenDir);
    row3->addWidget(m_openDir);
    outer->addLayout(row3);

    // 初始化屏幕列表（会顺便设好默认帧率）
    refreshScreenList();
    refreshRegionInfo();

    return m_setupCard;
}

QWidget *ScreenRecPanel::buildLiveCard()
{
    m_liveCard = new QFrame(this);
    m_liveCard->setObjectName(QStringLiteral("recLiveCard"));
    m_liveCard->setAttribute(Qt::WA_StyledBackground, true);
    // 高度跟着内容走：这张卡只有两行信息，让它跟着布局扩展会撑出一大块空白。
    m_liveCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    theme::applyStyle(m_liveCard, QStringLiteral(
        "QFrame#recLiveCard{background:@bg;border:1px solid @border;border-radius:10px;}"));

    auto *row = new QHBoxLayout(m_liveCard);
    row->setContentsMargins(18, 12, 18, 12);
    row->setSpacing(18);

    auto *left = new QVBoxLayout;
    left->setSpacing(2);
    left->addWidget(makeCaption(QStringLiteral("已录时长"), m_liveCard));

    m_time = new QLabel(QStringLiteral("00:00"), m_liveCard);
    m_time->setObjectName(QStringLiteral("recTime"));
    theme::applyStyle(m_time, QStringLiteral("font-size:28px;font-weight:600;color:@text;"));
    left->addWidget(m_time);

    m_stateText = new QLabel(m_liveCard);
    m_stateText->setObjectName(QStringLiteral("recStateText"));
    theme::applyStyle(m_stateText, QStringLiteral("font-size:12px;color:@text-muted;"));
    left->addWidget(m_stateText);
    row->addLayout(left, 1);

    m_detail = new QLabel(m_liveCard);
    m_detail->setObjectName(QStringLiteral("recDetail"));
    m_detail->setAlignment(Qt::AlignRight | Qt::AlignTop);
    theme::applyStyle(m_detail, QStringLiteral("font-size:12px;color:@text-muted;"));
    row->addWidget(m_detail, 0);

    return m_liveCard;
}

bool ScreenRecPanel::eventFilter(QObject *watched, QEvent *event)
{
    auto *box = qobject_cast<QComboBox *>(watched);
    if (box && m_arrows.contains(box)) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show: {
            // 与 qss 里 ::drop-down 的 18px 箭头区对齐；窗口拉伸、字体缩放后都要重摆
            QWidget *arrow = m_arrows.value(box);
            const QRect bounds = box->rect();
            arrow->move(bounds.width() - 18 + 4, (bounds.height() - arrow->height()) / 2);
            break;
        }
        case QEvent::EnabledChange:
            m_arrows.value(box)->update();   // 禁用时箭头跟着变淡
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenRecPanel::refreshScreenList()
{
    // 重建列表期间屏蔽信号：clear() 会发出 currentIndexChanged(-1)，
    // 那会让 applyDefaultFpsForScreen() 拿到主屏去算，把用户选的帧率改掉。
    const bool blocked = m_screenBox->blockSignals(true);
    const QString keep = m_screenBox->currentData().toString();
    m_screenBox->clear();

    QScreen *cursorScreen = QGuiApplication::screenAt(QCursor::pos());
    int selected = 0;
    int index = 0;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (!screen)
            continue;
        const QSize device = screenrec::devicePixelSize(screen);
        m_screenBox->addItem(QStringLiteral("%1（%2 × %3）")
                                 .arg(screen->name())
                                 .arg(device.width())
                                 .arg(device.height()),
                             screen->name());
        if (!keep.isEmpty() && screen->name() == keep)
            selected = index;
        else if (keep.isEmpty() && screen == cursorScreen)
            selected = index;
        ++index;
    }
    if (m_screenBox->count() > 0)
        m_screenBox->setCurrentIndex(selected);
    m_screenBox->blockSignals(blocked);

    applyDefaultFpsForScreen();
}

QScreen *ScreenRecPanel::selectedScreen() const
{
    const QString name = m_screenBox ? m_screenBox->currentData().toString() : QString();
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (screen && screen->name() == name)
            return screen;
    }
    return QGuiApplication::primaryScreen();
}

bool ScreenRecPanel::screenIsLarge() const
{
    QScreen *screen = selectedScreen();
    if (!screen)
        return false;
    const QSize device = screenrec::devicePixelSize(screen);
    return qint64(device.width()) * device.height() > 5000000;
}

void ScreenRecPanel::applyDefaultFpsForScreen()
{
    if (!m_fpsBox)
        return;
    // 大屏（超过 400 万像素）默认降到 10fps：抓屏本身就要 28ms 以上，
    // 15fps 的 66ms 预算里留给编码的余量太小，掉帧会很明显。
    const int target = screenIsLarge() ? 10 : 15;
    const int index = m_fpsBox->findData(target);
    if (index >= 0)
        m_fpsBox->setCurrentIndex(index);
}

void ScreenRecPanel::refreshRegionInfo()
{
    if (!m_regionInfo || !m_modeBox)
        return;

    QScreen *screen = selectedScreen();
    if (!screen) {
        m_regionInfo->setText(QStringLiteral("—"));
        return;
    }

    const bool wantRegion = m_modeBox->currentIndex() == 1;
    const bool haveRegion = !m_regionLogic.isEmpty()
                            && m_regionScreen == m_screenBox->currentData().toString();

    if (wantRegion && haveRegion) {
        // 显示**物理像素**：录出来的视频就是这个尺寸，显示逻辑像素会让用户在
        // 150% 缩放的屏幕上看到一个对不上的数字。
        const qreal dpr = screen->devicePixelRatio();
        m_regionInfo->setText(QStringLiteral("将录成 %1 × %2")
                                  .arg(qRound(m_regionLogic.width() * dpr))
                                  .arg(qRound(m_regionLogic.height() * dpr)));
        m_pickRegion->setText(QStringLiteral("重新框选…"));
    } else if (wantRegion) {
        m_regionInfo->setText(QStringLiteral("还没框选区域，当前会录整个屏幕"));
        m_pickRegion->setText(QStringLiteral("框选区域…"));
    } else {
        const QSize device = screenrec::devicePixelSize(screen);
        m_regionInfo->setText(QStringLiteral("将录成 %1 × %2")
                                  .arg(device.width())
                                  .arg(device.height()));
        m_pickRegion->setText(QStringLiteral("框选区域…"));
    }
}

void ScreenRecPanel::applyState(RecordService::State state)
{
    const bool idle = state == RecordService::State::Idle;
    const bool error = state == RecordService::State::Error;
    const bool recording = state == RecordService::State::Recording;
    const bool countdown = state == RecordService::State::Countdown;
    const bool stopping = state == RecordService::State::Stopping;

    const bool settingsEnabled = idle || error;

    m_screenBox->setEnabled(settingsEnabled);
    m_fpsBox->setEnabled(settingsEnabled);
    m_qualityBox->setEnabled(settingsEnabled);
    m_modeBox->setEnabled(settingsEnabled);
    m_pickRegion->setEnabled(settingsEnabled);
    m_cursorBox->setEnabled(settingsEnabled);
    m_chooseDir->setEnabled(settingsEnabled);

    // 录制中把设置区显示成"实际在用的参数"，避免用户以为改了就生效
    if (recording || countdown) {
        const RecordService::Config active = m_service->activeConfig();
        const int fpsIndex = m_fpsBox->findData(active.fps);
        if (fpsIndex >= 0)
            m_fpsBox->setCurrentIndex(fpsIndex);
        const int qualityIndex = m_qualityBox->findData(active.jpegQuality);
        if (qualityIndex >= 0)
            m_qualityBox->setCurrentIndex(qualityIndex);
    }

    const auto stats = m_service->stats();
    QString stateText;
    theme::Color stateColor = theme::Color::TextMuted;

    if (recording) {
        restylePrimaryButton(QStringLiteral("停止录制"), false, true, true);
        stateText = QStringLiteral("录制中 · %1 fps · %2 × %3")
                        .arg(stats.width > 0 ? QString::number(m_fpsBox->currentData().toInt())
                                             : QStringLiteral("--"))
                        .arg(stats.width)
                        .arg(stats.height);
        stateColor = theme::Color::Danger;
        theme::applyStyle(m_time, QStringLiteral("font-size:28px;font-weight:600;color:@danger;"));
        if (!m_liveTimer->isActive())
            m_liveTimer->start();
    } else if (countdown) {
        restylePrimaryButton(QStringLiteral("取消"), false, false, true);
        stateText = QStringLiteral("倒计时中，马上开始…");
        stateColor = theme::Color::Accent;
        theme::applyStyle(m_time, QStringLiteral("font-size:28px;font-weight:600;color:@text;"));
        if (!m_liveTimer->isActive())
            m_liveTimer->start();
    } else if (stopping) {
        restylePrimaryButton(QStringLiteral("正在保存…"), false, false, false);
        stateText = QStringLiteral("正在写入索引…");
        stateColor = theme::Color::TextMuted;
        if (!m_liveTimer->isActive())
            m_liveTimer->start();
    } else if (error) {
        restylePrimaryButton(QStringLiteral("重新录制"), true, false, true);
        stateText = m_service->lastError();
        stateColor = theme::Color::Danger;
        theme::applyStyle(m_time, QStringLiteral("font-size:28px;font-weight:600;color:@text;"));
        m_liveTimer->stop();
    } else {
        restylePrimaryButton(QStringLiteral("开始录制"), true, false, true);
        stateText = QStringLiteral("就绪 · 点开始后有 3 秒倒计时");
        stateColor = theme::Color::TextMuted;
        theme::applyStyle(m_time, QStringLiteral("font-size:28px;font-weight:600;color:@text;"));
        m_liveTimer->stop();
    }

    applyText(m_stateText, stateText, stateColor);
}

void ScreenRecPanel::restylePrimaryButton(const QString &text, bool accent, bool danger,
                                          bool enabled)
{
    m_startStop->setText(text);
    m_startStop->setEnabled(enabled);
    m_startStop->setProperty("accent", accent);

    // 只写尺寸不写颜色：颜色交给 qss 的 [accent="true"]；危险态才覆盖底色。
    if (danger) {
        theme::applyStyle(m_startStop,
                          QStringLiteral("QPushButton{min-height:38px;font-size:14px;padding:0 22px;"
                                         "background:@danger;border:1px solid @danger;"
                                         "color:@on-accent;font-weight:600;}"));
    } else {
        theme::applyStyle(m_startStop,
                          QStringLiteral("QPushButton{min-height:38px;font-size:14px;padding:0 22px;}"));
    }
    repolish(m_startStop);
}

void ScreenRecPanel::refreshLive()
{
    const auto stats = m_service->stats();
    const auto state = m_service->state();

    m_time->setText(formatDuration(stats.elapsedMs));

    if (state == RecordService::State::Recording || state == RecordService::State::Stopping) {
        m_detail->setText(
            QStringLiteral("已写 %1 帧 · 实际 %2 fps\n补帧 %3 · 丢帧 %4 · %5")
                .arg(stats.frames)
                .arg(stats.actualFps, 0, 'f', 1)
                .arg(stats.dups)
                .arg(stats.dropped + stats.skipped)
                .arg(formatBytes(stats.bytes)));
    } else {
        m_detail->setText(QStringLiteral("—"));
    }
}

RecordService::Config ScreenRecPanel::buildConfig() const
{
    RecordService::Config config;
    config.screenName = m_screenBox->currentData().toString();
    config.fps = m_fpsBox->currentData().toInt();
    config.jpegQuality = m_qualityBox->currentData().toInt();
    config.drawCursor = m_cursorBox->isChecked();
    config.countdown = true;
    config.outputDir = m_outputDir;

    const bool haveRegion = !m_regionLogic.isEmpty()
                            && m_regionScreen == m_screenBox->currentData().toString();
    if (m_modeBox->currentIndex() == 1 && haveRegion) {
        config.mode = RecordService::Config::Mode::Region;
        config.regionLogic = m_regionLogic;
    } else {
        // 选了"自定义区域"却没框过，按整屏录 —— 总比报错让用户卡住好，
        // 面板上的提示文字已经说明了这一点。
        config.mode = RecordService::Config::Mode::FullScreen;
    }
    return config;
}

void ScreenRecPanel::onStartStop()
{
    switch (m_service->state()) {
    case RecordService::State::Recording:
        m_service->stop();
        return;
    case RecordService::State::Countdown:
        m_service->cancelCountdown();
        return;
    case RecordService::State::Stopping:
        return;
    default:
        break;
    }

    QString error;
    if (!m_service->start(buildConfig(), &error)) {
        setStatus(error, theme::Color::Danger);
        return;
    }
    setStatus(QStringLiteral("准备录制…"), theme::Color::Accent);
}

void ScreenRecPanel::onStateChanged(RecordService::State state)
{
    applyState(state);
    if (state == RecordService::State::Recording) {
        setStatus(QStringLiteral("正在录制 · 切到其他页面不会中断"), theme::Color::SuccessText);
    } else if (state == RecordService::State::Stopping) {
        setStatus(QStringLiteral("正在保存，请稍候…"), theme::Color::TextMuted);
    }
    refreshLive();
}

void ScreenRecPanel::onFinished(const QString &path)
{
    m_lastFile = path;
    m_openFile->setVisible(true);
    m_convert->setVisible(m_hasFfmpeg);
    setStatus(QStringLiteral("已保存：%1").arg(QFileInfo(path).fileName()),
              theme::Color::SuccessText);
    refreshLive();
}

void ScreenRecPanel::onFailed(const QString &message)
{
    setStatus(message, theme::Color::Danger);
    refreshLive();
}

void ScreenRecPanel::setStatus(const QString &text, theme::Color token)
{
    applyText(m_status, text, token);
}

void ScreenRecPanel::onPickRegion()
{
    if (m_regionOverlay)
        return;   // 已经在框选

    QScreen *screen = selectedScreen();
    if (!screen) {
        setStatus(QStringLiteral("找不到可用的屏幕"), theme::Color::Danger);
        return;
    }

    // 隐藏宿主窗口：不隐藏的话，浮层底图上看到的是"被自己窗口挡住"的画面，
    // 用户没法框选窗口后面/下面的区域（OCR 的截图流程是同样的做法）。
    QPointer<QWidget> host = window();
    const bool hostWasVisible = host && host->isVisible();
    if (host)
        host->hide();

    QPointer<ScreenRecPanel> self(this);
    const auto restoreHost = [host, hostWasVisible] {
        if (host && hostWasVisible) {
            host->show();
            host->raise();
            host->activateWindow();
        }
    };

    // hide() 之后桌面要重绘一轮，立刻抓屏会截到主窗口残影。
    // 以 this 为 context：面板若在等待期间被销毁，这个回调自动作废。
    QTimer::singleShot(260, this, [this, self, screen, restoreHost] {
        if (!self)
            return;

        const QRect geometry = screen->geometry();
        QPixmap shot = screen->grabWindow(0);
        if (shot.isNull()) {
            restoreHost();
            setStatus(screenrec::grabFailureHint(), theme::Color::Danger);
            return;
        }
        // 不猜 platform 的 DPR 行为，用「截图像素宽 / 屏幕逻辑宽」反推
        const qreal dpr = geometry.width() > 0
                              ? qMax(1.0, qreal(shot.width()) / qreal(geometry.width()))
                              : 1.0;
        shot.setDevicePixelRatio(dpr);

        const QString screenName = m_screenBox->currentData().toString();
        auto *overlay = new RegionSelectOverlay(
            geometry, shot, dpr,
            [self, restoreHost, screenName](const QRect &region) {
                restoreHost();
                if (!self)
                    return;
                self->m_regionLogic = region;
                self->m_regionScreen = screenName;
                // 框完自动切到"自定义区域"，否则用户会疑惑"我框了怎么还是整屏"
                self->m_modeBox->setCurrentIndex(1);
                self->refreshRegionInfo();
                self->setStatus(QStringLiteral("已选择区域，点「开始录制」"), theme::Color::SuccessText);
            },
            [self, restoreHost] {
                restoreHost();
                if (self)
                    self->setStatus(QStringLiteral("已取消框选"), theme::Color::TextFaint);
            });
        m_regionOverlay = overlay;
        overlay->show();
        overlay->raise();
        overlay->activateWindow();
        overlay->setFocus();   // Esc 取消依赖浮层拿到键盘焦点
    });
}

void ScreenRecPanel::onChooseDir()
{
    const QString picked = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择录制文件的保存目录"), m_outputDir);
    if (picked.isEmpty())
        return;

    m_outputDir = picked;
    const QDir dir(picked);
    const QString leaf = dir.dirName();
    const QString parent = QFileInfo(picked).dir().dirName();
    m_outDirLabel->setText(parent.isEmpty() ? leaf : parent + QStringLiteral("/") + leaf);
    m_outDirLabel->setToolTip(picked);
    setStatus(QStringLiteral("输出目录已改为 %1").arg(picked), theme::Color::TextMuted);
}

void ScreenRecPanel::onOpenDir()
{
    QDir().mkpath(m_outputDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_outputDir));
}

void ScreenRecPanel::onOpenFile()
{
    if (!m_lastFile.isEmpty() && QFile::exists(m_lastFile)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastFile));
        return;
    }
    onOpenDir();
}

void ScreenRecPanel::onConvert()
{
    if (m_lastFile.isEmpty() || !QFile::exists(m_lastFile) || m_convertProcess)
        return;

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        setStatus(QStringLiteral("找不到 ffmpeg，无法转码"), theme::Color::Danger);
        return;
    }

    QString output = m_lastFile;
    output.chop(4);   // 去掉 ".avi"
    output += QStringLiteral(".mp4");
    QFile::remove(output);

    const QStringList arguments{
        QStringLiteral("-y"),
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-i"),
        m_lastFile,
        // 区域录制会出现奇数宽高，H.264 的 yuv420p 要求两边都是偶数，
        // 不 pad 会直接失败（"height not divisible by 2"）。
        QStringLiteral("-vf"),
        QStringLiteral("pad=ceil(iw/2)*2:ceil(ih/2)*2"),
        QStringLiteral("-c:v"),
        QStringLiteral("libx264"),
        QStringLiteral("-preset"),
        QStringLiteral("veryfast"),
        QStringLiteral("-crf"),
        QStringLiteral("20"),
        QStringLiteral("-pix_fmt"),
        QStringLiteral("yuv420p"),
        output,
    };

    m_convertProcess = new QProcess(this);
    m_convertProcess->setProcessChannelMode(QProcess::SeparateChannels);
    m_convert->setEnabled(false);
    setStatus(QStringLiteral("正在转码…（大文件需要几十秒，关掉本页会中断）"),
              theme::Color::Accent);

    connect(m_convertProcess, &QProcess::finished, this,
            [this, output](int exitCode, QProcess::ExitStatus status) {
                const QString stdErr = QString::fromLocal8Bit(
                    m_convertProcess->readAllStandardError()).trimmed();
                m_convertProcess->deleteLater();
                m_convertProcess = nullptr;
                m_convert->setEnabled(true);

                if (status == QProcess::NormalExit && exitCode == 0 && QFile::exists(output)) {
                    m_lastFile = output;
                    m_openFile->setVisible(true);
                    setStatus(QStringLiteral("已转成 MP4：%1").arg(QFileInfo(output).fileName()),
                              theme::Color::SuccessText);
                } else {
                    // ffmpeg 的第一行报错通常就够定位，只显示末尾一小段避免刷屏
                    const QString detail = stdErr.isEmpty()
                                               ? QStringLiteral("退出码 %1").arg(exitCode)
                                               : stdErr.right(300);
                    setStatus(QStringLiteral("转码失败：%1").arg(detail), theme::Color::Danger);
                }
            });
    connect(m_convertProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_convertProcess)
            return;
        m_convertProcess->deleteLater();
        m_convertProcess = nullptr;
        m_convert->setEnabled(true);
        setStatus(QStringLiteral("无法启动 ffmpeg"), theme::Color::Danger);
    });

    m_convertProcess->start(ffmpeg, arguments);
}

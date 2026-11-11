#include "SettingsPage.h"

#include "ITool.h"
#include "LayoutMetrics.h"
#include "Log.h"
#include "PluginManager.h"
#include "Settings.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
// qt_windows.h 已经处理好 NOMINMAX / WIN32_LEAN_AND_MEAN 之类的开关，
// 直接 include <windows.h> 会把 min/max 宏带进来跟 Qt 打架。
#  include <QtCore/qt_windows.h>
#endif

namespace {

/// 给 hint 类标签切换状态色（ok / error / 空=默认）。
///
/// 为什么不用 setStyleSheet：内联样式的优先级高于全局样式表，写死颜色后
/// 用户切到深色主题时会停在旧的浅色值上；而且每处都要再把字号抄一遍。
/// 动态属性 + unpolish/polish 让 qss 重新求值，颜色由主题决定。
void setHintState(QWidget *widget, const QString &state)
{
    if (!widget)
        return;
    widget->setProperty("state", state);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

} // namespace

// ---------------------------------------------------------------- HotkeyButton

HotkeyButton::HotkeyButton(QWidget *parent)
    : QPushButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setMinimumWidth(180);
    connect(this, &QPushButton::clicked, this, &HotkeyButton::startCapture);
}

void HotkeyButton::setSequence(const QKeySequence &seq)
{
    m_sequence = seq;
    setText(seq.isEmpty() ? QStringLiteral("未设置") : seq.toString(QKeySequence::NativeText));
}

void HotkeyButton::startCapture()
{
    m_capturing = true;
    setText(QStringLiteral("请按下新的组合键…"));
    setFocus(Qt::OtherFocusReason);   // 拿焦点才能收到 keyPressEvent
}

void HotkeyButton::stopCapture()
{
    m_capturing = false;
    setSequence(m_sequence);   // 放弃捕获，恢复原值
}

void HotkeyButton::keyPressEvent(QKeyEvent *event)
{
    if (!m_capturing) {
        QPushButton::keyPressEvent(event);
        return;
    }

    const int key = event->key();

    // Esc = 取消捕获（而不是把 Esc 存成热键）
    if (key == Qt::Key_Escape) {
        stopCapture();
        return;
    }

    // 只按下修饰键时继续等真正的按键，不要把 Ctrl 单独当成热键
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt
        || key == Qt::Key_Meta || key == Qt::Key_unknown) {
        return;
    }

    // 只保留四个修饰位，其余（如 KeypadModifier）对全局热键无意义
    const Qt::KeyboardModifiers mods = event->modifiers()
                                       & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier
                                          | Qt::MetaModifier);
    acceptSequence(mods, key);
}

void HotkeyButton::acceptSequence(Qt::KeyboardModifiers mods, int key)
{
    if (mods == Qt::NoModifier) {
        // 裸键会被 RegisterHotKey/XGrabKey 拒绝，提前在这里提示，文案与后端保持一致
        setText(QStringLiteral("至少要一个 Ctrl/Alt/Shift/Meta"));
        return;
    }

    m_sequence = QKeySequence(QKeyCombination(mods, static_cast<Qt::Key>(key)));
    m_capturing = false;
    setSequence(m_sequence);
    emit sequenceCaptured(m_sequence);
}

#ifdef Q_OS_WIN
bool HotkeyButton::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    // Windows 把 Alt+Space 保留给窗口系统菜单（SC_KEYMENU）。Qt 收到
    // WM_SYSKEYDOWN(VK_SPACE) 后就地把它转成菜单命令，**不会生成 QKeyEvent**，
    // 于是 keyPressEvent 永远等不到这一次按键：用户按下 Alt+Space 时按钮一直停在
    // 「请按下新的组合键…」，系统菜单弹出还可能把焦点抢走 → focusOutEvent 又把值
    // 恢复成旧的。表现就是「录 Alt+Space 毫无反应」，而 RegisterHotKey 其实完全
    // 支持这个组合（实测注册成功且能收到 WM_HOTKEY）。
    //
    // 所以这里在原生消息层把它拦下来：自己读修饰键状态换算组合，并吞掉消息，
    // 免得真的弹出窗口菜单。只在捕获状态、且确实是 Space 时介入，其它按键仍走
    // Qt 的正常派发路径（keyPressEvent），不改变已有行为。
    if (m_capturing && eventType == QByteArrayLiteral("windows_generic_MSG")) {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == WM_SYSKEYDOWN && msg->wParam == VK_SPACE) {
            Qt::KeyboardModifiers mods;
            if (GetKeyState(VK_MENU) & 0x8000)
                mods |= Qt::AltModifier;
            if (GetKeyState(VK_CONTROL) & 0x8000)
                mods |= Qt::ControlModifier;
            if (GetKeyState(VK_SHIFT) & 0x8000)
                mods |= Qt::ShiftModifier;
            if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
                mods |= Qt::MetaModifier;

            if (mods != Qt::NoModifier) {
                acceptSequence(mods, Qt::Key_Space);
                if (result)
                    *result = 0;
                return true;
            }
        }
    }
    return QPushButton::nativeEvent(eventType, message, result);
}
#endif

void HotkeyButton::focusOutEvent(QFocusEvent *event)
{
    // 点到别处就放弃捕获，否则按钮会卡在"请按下…"状态
    if (m_capturing)
        stopCapture();
    QPushButton::focusOutEvent(event);
}

// ---------------------------------------------------------------- SettingsPage

SettingsPage::SettingsPage(Settings *settings, PluginManager *plugins, QWidget *parent)
    : QWidget(parent)
    , m_settings(settings)
    , m_plugins(plugins)
{
    // 设置页包一层滚动区：四个分组 + 边距在窄窗口 / 高 DPI 下可能超过主窗口最小高度
    // （580），不滚动的话「关于」组会被直接裁掉。滚动区只在内容放不下时才出现滚动条，
    // 平时视觉上和以前直接堆叠一样（透明底、无边框）。
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    scroll->setWidgetResizable(true);            // 内容宽度跟随 viewport，否则横向也会出现滚动条
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("settingsContent"));

    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(xtools::layout::kPageMargin, 20, xtools::layout::kPageMargin, 24);
    layout->setSpacing(16);

    layout->addWidget(buildGeneralGroup());
    layout->addWidget(buildPluginGroup());
    layout->addWidget(buildUpdateGroup());
    layout->addWidget(buildAboutGroup());
    layout->addStretch(1);

    scroll->setWidget(content);
    outer->addWidget(scroll);
}

QWidget *SettingsPage::buildGeneralGroup()
{
    auto *group = new QGroupBox(QStringLiteral("常规"), this);
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(16, 18, 16, 16);
    layout->setSpacing(12);

    auto *hotkeyRow = new QHBoxLayout;

    auto *hotkeyLabel = new QLabel(QStringLiteral("呼出搜索热键"), group);
    hotkeyLabel->setMinimumWidth(110);

    m_hotkeyButton = new HotkeyButton(group);
    m_hotkeyButton->setSequence(m_settings->hotkey());

    m_hotkeyStatus = new QLabel(group);
    // 颜色与字号统一由 style.qss 的 QLabel#hint 决定（换肤时才会跟着变）
    m_hotkeyStatus->setObjectName(QStringLiteral("hint"));

    hotkeyRow->addWidget(hotkeyLabel);
    hotkeyRow->addWidget(m_hotkeyButton);
    hotkeyRow->addWidget(m_hotkeyStatus, 1);
    layout->addLayout(hotkeyRow);

    connect(m_hotkeyButton, &HotkeyButton::sequenceCaptured, this, [this](const QKeySequence &seq) {
        // 先落盘再交给上层注册：注册失败时配置里也已经是用户选的组合，
        // 下次启动会再试一次，而不是"重启后热键又变回旧的"
        m_settings->setHotkey(seq);
        emit hotkeyChanged(seq);
    });

    auto *autostart = new QCheckBox(QStringLiteral("开机自动启动"), group);
    autostart->setChecked(m_settings->startWithSystem());
    connect(autostart, &QCheckBox::toggled, this, [this](bool on) {
        m_settings->setStartWithSystem(on);
    });
    layout->addWidget(autostart);

    // ---- 界面主题
    auto *themeRow = new QHBoxLayout;

    auto *themeLabel = new QLabel(QStringLiteral("界面主题"), group);
    themeLabel->setMinimumWidth(110);   // 与热键那一行的标签对齐

    m_themeCombo = new QComboBox(group);
    // 用户文案在前、配置值放 data：配置值是 ini 里存的内容，必须稳定（不随文案翻译变）
    m_themeCombo->addItem(QStringLiteral("跟随系统"), Theme::modeToString(Theme::Mode::System));
    m_themeCombo->addItem(QStringLiteral("浅色"), Theme::modeToString(Theme::Mode::Light));
    m_themeCombo->addItem(QStringLiteral("深色"), Theme::modeToString(Theme::Mode::Dark));
    m_themeCombo->setCurrentIndex(m_themeCombo->findData(Theme::modeToString(Theme::instance().mode())));

    auto *themeHint = new QLabel(QStringLiteral("切换后立即生效，不用重启"), group);
    themeHint->setObjectName(QStringLiteral("hint"));

    themeRow->addWidget(themeLabel);
    themeRow->addWidget(m_themeCombo);
    themeRow->addWidget(themeHint, 1);
    layout->addLayout(themeRow);

    connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString value = m_themeCombo->itemData(index).toString();
        bool ok = false;
        const Theme::Mode mode = Theme::modeFromString(value, &ok);
        if (!ok)
            return;
        // setMode 内部负责落盘 + 重新应用；这里不要重复调 apply()
        Theme::instance().setMode(mode);
    });

    return group;
}

QWidget *SettingsPage::buildPluginGroup()
{
    auto *group = new QGroupBox(QStringLiteral("工具与插件"), this);
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(16, 18, 16, 16);
    layout->setSpacing(10);

    const QString dirPath = m_plugins ? m_plugins->pluginDir() : QString();

    auto *dirRow = new QHBoxLayout;
    auto *dirLabel = new QLabel(QDir::toNativeSeparators(dirPath), group);
    dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);   // 方便复制路径去丢文件
    dirLabel->setObjectName(QStringLiteral("settingsValue"));

    auto *openBtn = new QPushButton(QStringLiteral("打开目录"), group);
    auto *restartBtn = new QPushButton(QStringLiteral("重启应用"), group);
    restartBtn->setProperty("accent", true);

    dirRow->addWidget(new QLabel(QStringLiteral("插件目录"), group));
    dirRow->addWidget(dirLabel, 1);
    dirRow->addWidget(openBtn);
    dirRow->addWidget(restartBtn);
    layout->addLayout(dirRow);

    connect(openBtn, &QPushButton::clicked, this, [dirPath] {
        QDir().mkpath(dirPath);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath));
    });
    // 不做热重载（见 PluginManager 注释），"重新扫描"就是重启
    connect(restartBtn, &QPushButton::clicked, this, &SettingsPage::restartRequested);

    const int loaded = m_plugins ? m_plugins->loadedCount() : 0;
    const QStringList errors = m_plugins ? m_plugins->errors() : QStringList{};

    auto *summary = new QLabel(QStringLiteral("已加载 %1 个插件，失败 %2 个").arg(loaded).arg(errors.size()),
                              group);
    summary->setObjectName(QStringLiteral("settingsValue"));
    layout->addWidget(summary);

    // 失败原因逐条列出：用户丢进来的插件加载不了时，这里是他唯一能看到的诊断信息
    for (const QString &err : errors) {
        auto *line = new QLabel(QStringLiteral("• %1").arg(err), group);
        line->setWordWrap(true);
        line->setObjectName(QStringLiteral("settingsError"));
        layout->addWidget(line);
    }

    auto *tip = new QLabel(
        QStringLiteral("离线安装：把编译好的插件动态库复制到上面的目录，重启应用即可生效；删除即卸载。"),
        group);
    tip->setWordWrap(true);
    tip->setObjectName(QStringLiteral("hint"));
    layout->addWidget(tip);

    return group;
}

QWidget *SettingsPage::buildUpdateGroup()
{
    auto *group = new QGroupBox(QStringLiteral("更新"), this);
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(16, 18, 16, 16);
    layout->setSpacing(10);

    auto *row = new QHBoxLayout;
    row->setAlignment(Qt::AlignVCenter);

    auto *current = new QLabel(QStringLiteral("当前版本 %1").arg(QApplication::applicationVersion()), group);
    current->setObjectName(QStringLiteral("settingsValue"));
    // 显式垂直居中：行高由右侧按钮撑起来，不写这一句文字会贴在行顶
    current->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_updateButton = new QPushButton(QStringLiteral("检查更新"), group);
    m_updateButton->setProperty("accent", true);

    row->addWidget(current);
    row->addStretch(1);
    row->addWidget(m_updateButton);
    layout->addLayout(row);

    m_autoCheck = new QCheckBox(QStringLiteral("启动时自动检查更新（每天最多一次）"), group);
    m_autoCheck->setChecked(m_settings ? m_settings->autoCheckUpdate() : true);
    layout->addWidget(m_autoCheck);

    m_updateStatus = new QLabel(group);
    m_updateStatus->setWordWrap(true);
    m_updateStatus->setObjectName(QStringLiteral("hint"));
    // 没有状态就别占一行：空 QLabel 会在勾选框和提示之间留出一道莫名其妙的空隙
    m_updateStatus->hide();
    layout->addWidget(m_updateStatus);

    auto *tip = new QLabel(
        QStringLiteral("更新包来自 GitHub Releases。安装版下载后会自动静默升级；"
                       "便携版只下载压缩包，解压覆盖由你决定。"),
        group);
    tip->setWordWrap(true);
    tip->setObjectName(QStringLiteral("hint"));
    layout->addWidget(tip);

    // 立刻置灰而不是等信号回来：用户连点两下会压出两个请求，
    // 后到的结果还可能覆盖先到的（例如"已是最新"盖掉"发现新版本"）。
    connect(m_updateButton, &QPushButton::clicked, this, [this] {
        m_updateButton->setEnabled(false);
        setUpdateStatus(QStringLiteral("正在检查更新…"), true);
        emit checkUpdateRequested();
    });
    connect(m_autoCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_settings)
            m_settings->setAutoCheckUpdate(on);
    });

    return group;
}

void SettingsPage::setUpdateStatus(const QString &text, bool ok)
{
    m_updateButton->setEnabled(true);
    m_updateStatus->setText(text);
    setHintState(m_updateStatus, ok ? QStringLiteral("ok") : QStringLiteral("error"));
    m_updateStatus->setVisible(!text.isEmpty());
}

void SettingsPage::setUpdateChecking(bool checking)
{
    m_updateButton->setEnabled(!checking);
    if (checking) {
        m_updateStatus->setText(QStringLiteral("正在检查更新…"));
        setHintState(m_updateStatus, QString());
        m_updateStatus->show();
    }
}

QWidget *SettingsPage::buildAboutGroup()
{
    auto *group = new QGroupBox(QStringLiteral("关于"), this);
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(16, 18, 16, 16);
    layout->setSpacing(6);

    auto *ver = new QLabel(QStringLiteral("CXK 工具箱 %1").arg(QApplication::applicationVersion()), group);
    ver->setObjectName(QStringLiteral("settingsVersion"));
    layout->addWidget(ver);

    auto *qt = new QLabel(QStringLiteral("Qt %1 · 接口版本 %2")
                              .arg(QString::fromLatin1(qVersion()))
                              .arg(XTOOLS_API_VERSION),
                          group);
    qt->setObjectName(QStringLiteral("hint"));
    layout->addWidget(qt);

    auto *cfg = new QLabel(QStringLiteral("配置文件：%1").arg(QDir::toNativeSeparators(m_settings->configFilePath())),
                           group);
    cfg->setWordWrap(true);
    cfg->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cfg->setObjectName(QStringLiteral("hint"));
    layout->addWidget(cfg);

    // 插件加载失败时用户常被问"日志在哪"，直接给路径最省事
    auto *logLabel = new QLabel(QStringLiteral("日志：%1").arg(QDir::toNativeSeparators(Log::filePath())), group);
    logLabel->setWordWrap(true);
    logLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logLabel->setObjectName(QStringLiteral("hint"));
    layout->addWidget(logLabel);

    return group;
}

void SettingsPage::setHotkeyStatus(const QString &text, bool ok)
{
    if (!m_hotkeyStatus)
        return;
    m_hotkeyStatus->setText(text);
    setHintState(m_hotkeyStatus, ok ? QStringLiteral("ok") : QStringLiteral("error"));
}

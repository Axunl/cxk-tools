#include "SettingsPage.h"

#include "ITool.h"
#include "Log.h"
#include "PluginManager.h"
#include "Settings.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

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
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 20, 28, 24);
    layout->setSpacing(16);

    layout->addWidget(buildGeneralGroup());
    layout->addWidget(buildPluginGroup());
    layout->addWidget(buildAboutGroup());
    layout->addStretch(1);
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
    m_hotkeyStatus->setObjectName(QStringLiteral("hint"));
    m_hotkeyStatus->setStyleSheet(QStringLiteral("color:#94A3B8;font-size:12px;"));

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
    dirLabel->setStyleSheet(QStringLiteral("color:#475569;font-size:12px;"));

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
    summary->setStyleSheet(QStringLiteral("color:#475569;font-size:12px;"));
    layout->addWidget(summary);

    // 失败原因逐条列出：用户丢进来的插件加载不了时，这里是他唯一能看到的诊断信息
    for (const QString &err : errors) {
        auto *line = new QLabel(QStringLiteral("• %1").arg(err), group);
        line->setWordWrap(true);
        line->setStyleSheet(QStringLiteral("color:#DC2626;font-size:12px;"));
        layout->addWidget(line);
    }

    auto *tip = new QLabel(
        QStringLiteral("离线安装：把编译好的插件动态库复制到上面的目录，重启应用即可生效；删除即卸载。"),
        group);
    tip->setWordWrap(true);
    tip->setStyleSheet(QStringLiteral("color:#94A3B8;font-size:12px;"));
    layout->addWidget(tip);

    return group;
}

QWidget *SettingsPage::buildAboutGroup()
{
    auto *group = new QGroupBox(QStringLiteral("关于"), this);
    auto *layout = new QVBoxLayout(group);
    layout->setContentsMargins(16, 18, 16, 16);
    layout->setSpacing(6);

    auto *ver = new QLabel(QStringLiteral("CXK 工具箱 %1").arg(QApplication::applicationVersion()), group);
    ver->setStyleSheet(QStringLiteral("color:#0F172A;font-weight:600;"));
    layout->addWidget(ver);

    auto *qt = new QLabel(QStringLiteral("Qt %1 · 接口版本 %2")
                              .arg(QString::fromLatin1(qVersion()))
                              .arg(XTOOLS_API_VERSION),
                          group);
    qt->setStyleSheet(QStringLiteral("color:#94A3B8;font-size:12px;"));
    layout->addWidget(qt);

    auto *cfg = new QLabel(QStringLiteral("配置文件：%1").arg(QDir::toNativeSeparators(m_settings->configFilePath())),
                           group);
    cfg->setWordWrap(true);
    cfg->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cfg->setStyleSheet(QStringLiteral("color:#94A3B8;font-size:12px;"));
    layout->addWidget(cfg);

    // 插件加载失败时用户常被问"日志在哪"，直接给路径最省事
    auto *logLabel = new QLabel(QStringLiteral("日志：%1").arg(QDir::toNativeSeparators(Log::filePath())), group);
    logLabel->setWordWrap(true);
    logLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    logLabel->setStyleSheet(QStringLiteral("color:#94A3B8;font-size:12px;"));
    layout->addWidget(logLabel);

    return group;
}

void SettingsPage::setHotkeyStatus(const QString &text, bool ok)
{
    if (!m_hotkeyStatus)
        return;
    m_hotkeyStatus->setText(text);
    m_hotkeyStatus->setStyleSheet(QStringLiteral("font-size:12px;color:%1;")
                                      .arg(ok ? QStringLiteral("#10B981") : QStringLiteral("#DC2626")));
}

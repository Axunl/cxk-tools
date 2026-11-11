#include "Theme.h"

#include "Log.h"
#include "Settings.h"

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QStyleHints>
#include <QVariant>
#include <QWidget>

namespace {

using xtools::theme::Color;

/// 用当前色板拼一份 QPalette。
///
/// 为什么非要有：qss 只覆盖我们自己画的部分，原生样式绘制的东西（菜单圆角外的
/// 阴影、滚动条箭头、QMessageBox、系统文件对话框、QComboBox 弹出层的滚动条）
/// 一律读调色板。不设它，深色模式下这些地方会白得刺眼。
QPalette buildPalette()
{
    QPalette p;

    const QColor bg = xtools::theme::color(Color::Bg);
    const QColor text = xtools::theme::color(Color::Text);
    const QColor disabled = xtools::theme::color(Color::TextDisabled);

    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, bg);
    p.setColor(QPalette::AlternateBase, xtools::theme::color(Color::BgAlt));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, xtools::theme::color(Color::TextFaint));
    p.setColor(QPalette::Button, bg);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, xtools::theme::color(Color::OnAccent));
    p.setColor(QPalette::Highlight, xtools::theme::color(Color::Accent));
    p.setColor(QPalette::HighlightedText, xtools::theme::color(Color::OnAccent));
    p.setColor(QPalette::Link, xtools::theme::color(Color::Accent));
    p.setColor(QPalette::LinkVisited, xtools::theme::color(Color::AccentText));

    // 3D 边框类角色：原生样式用它画分组框、分隔线，不调的话深色底上会出现
    // 一圈浅灰高光（看起来像没对齐的描边）。
    p.setColor(QPalette::Light, xtools::theme::color(Color::BorderSubtle));
    p.setColor(QPalette::Midlight, xtools::theme::color(Color::Border));
    p.setColor(QPalette::Mid, xtools::theme::color(Color::BorderStrong));
    p.setColor(QPalette::Dark, xtools::theme::color(Color::BorderStrong));
    p.setColor(QPalette::Shadow, xtools::theme::color(Color::BorderStrong));

    p.setColor(QPalette::ToolTipBase, xtools::theme::color(Color::TooltipBg));
    p.setColor(QPalette::ToolTipText, xtools::theme::color(Color::TooltipText));

    // 禁用态：只改文字与高亮，底色保持主表面。禁用按钮若也用灰底，
    // 会和"输入框下沉"的视觉混在一起（QSS 里已经单独处理了按钮）。
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, xtools::theme::color(Color::Border));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, xtools::theme::color(Color::TextFaint));

    return p;
}

} // namespace

Theme &Theme::instance()
{
    static Theme theme;
    return theme;
}

Theme::Theme(QObject *parent)
    : QObject(parent)
{
}

QString Theme::modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Light:
        return QStringLiteral("light");
    case Mode::Dark:
        return QStringLiteral("dark");
    case Mode::System:
        break;
    }
    return QStringLiteral("system");
}

Theme::Mode Theme::modeFromString(const QString &text, bool *ok)
{
    const QString value = text.trimmed().toLower();
    if (ok)
        *ok = true;
    if (value == QLatin1String("light"))
        return Mode::Light;
    if (value == QLatin1String("dark"))
        return Mode::Dark;
    if (value.isEmpty() || value == QLatin1String("system"))
        return Mode::System;
    if (ok)
        *ok = false;
    return Mode::System;
}

bool Theme::systemPrefersDark() const
{
    // Qt 6.5 起跨平台可用：Windows 读应用主题、macOS 读外观、Linux 读门户设置。
    // 平台未上报时返回 Unknown —— 此时宁可按浅色，也不要在用户没选深色时突然变黑。
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

bool Theme::isDark() const
{
    switch (m_mode) {
    case Mode::Dark:
        return true;
    case Mode::Light:
        return false;
    case Mode::System:
        break;
    }
    return systemPrefersDark();
}

void Theme::load()
{
    bool ok = true;
    m_mode = modeFromString(Settings::instance().themeMode(), &ok);
    if (!ok) {
        // 手工把 ini 改坏时回到"跟随系统"，而不是让程序起不来或卡在浅色
        qWarning() << "Theme: 无法识别的主题配置，回退为跟随系统";
        m_mode = Mode::System;
    }

    // 系统配色变化（Windows/macOS 的"浅色/深色"开关）只在 System 模式下生效。
    // 注意 Qt 也可能因我们调用 setColorScheme() 而发这个信号，apply() 里用
    // m_applying 做了防重入。
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) {
                if (m_mode == Mode::System)
                    apply();
            });
}

void Theme::setMode(Mode mode, bool persist)
{
    m_mode = mode;
    if (persist)
        Settings::instance().setThemeMode(modeToString(mode));
    apply();
}

QString Theme::styleTemplate()
{
    if (!m_template.isEmpty())
        return m_template;

    QFile file(QStringLiteral(":/style.qss"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // 样式缺失不致命（界面退回系统默认外观），但必须留痕
        qWarning() << "Theme: 无法加载样式表 :/style.qss";
        return QString();
    }
    // 显式 fromUtf8：源码是 UTF-8，不能让 Qt 按 locale 猜
    m_template = QString::fromUtf8(file.readAll());
    return m_template;
}

void Theme::apply()
{
    // 防重入：setColorScheme() 可能同步发 colorSchemeChanged，我们的槽又会调
    // apply()。没有这个闸门就会无限递归（栈溢出，且日志一片空白）。
    static bool applying = false;
    if (applying)
        return;
    applying = true;

    // 1) 先把"请求的系统配色"复位/写死，再读色值。
    //    顺序很重要：从"深色"切回"跟随系统"时，若先读再复位，读到的还是上一次
    //    我们强制写进去的 Dark，结果就是"选了跟随系统却仍停在深色"。
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (m_mode == Mode::System) {
        // Unknown = 交还给平台，colorScheme() 随后反映系统的真实设置
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Unknown);
    }
#endif

    const bool dark = isDark();

    // 2) 写标记：色板查询、插件里的 resolve() 都以它为准
    qApp->setProperty(xtools::theme::kDarkProperty, dark);

    // 3) 让原生窗口也跟着换（标题栏、系统对话框）
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (m_mode != Mode::System)
        QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark
                                                          : Qt::ColorScheme::Light);
#endif

    // 4) 调色板 + 全局样式表
    qApp->setPalette(buildPalette());
    const QString qss = xtools::theme::resolve(styleTemplate());
    if (qss.isEmpty())
        qWarning() << "Theme: 样式表为空，界面将使用系统默认外观";
    qApp->setStyleSheet(qss);

    // 5) 插件面板里的内联样式：模板存在控件属性上，这里逐控件重算。
    //    只重设真正带模板的控件，避免给所有控件叠一层空样式（会打断继承）。
    const QWidgetList widgets = QApplication::allWidgets();
    for (QWidget *widget : widgets) {
        if (!widget)
            continue;
        const QVariant pattern = widget->property(xtools::theme::kStyleProperty);
        if (pattern.isValid())
            widget->setStyleSheet(xtools::theme::resolve(pattern.toString()));
        // 自绘控件（QPainter 里直接取色）不吃样式表，只能触发一次重绘
        widget->update();
    }

    applying = false;
    qInfo() << "Theme: applied" << modeToString(m_mode) << (dark ? "dark" : "light");
    emit themeChanged();
}

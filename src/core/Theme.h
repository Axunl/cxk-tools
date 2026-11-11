#pragma once

// ---------------------------------------------------------------------------
// 界面主题：跟随系统 / 强制浅色 / 强制深色。
//
// 换肤不是"换一张样式表"这么简单，落地要动三处，缺一处就会留下色块：
//   1) QPalette —— 原生样式绘制的部分（菜单、滚动条箭头、QMessageBox、
//      文件对话框）不吃 qss，只能靠调色板；
//   2) style.qss —— 把里面的 @token 按当前色板解析后整体重设；
//   3) 插件里的内联样式 —— 插件是独立动态库，宿主看不到它拼的字符串，
//      于是约定「模板挂在控件的动态属性上」（xtools::theme::applyStyle），
//      换肤时遍历 QApplication::allWidgets() 逐个重算。
//
// 配置项：settings/theme = system | light | dark（默认 system）。
// 命令行 --theme <值> 只覆盖本次运行，不写回配置（截图回归要用它）。
// ---------------------------------------------------------------------------

#include "ThemePalette.h"

#include <QObject>
#include <QString>

class Theme : public QObject
{
    Q_OBJECT

public:
    enum class Mode
    {
        System,   ///< 跟随系统（读 QStyleHints::colorScheme，系统切换时自动跟随）
        Light,
        Dark,
    };
    Q_ENUM(Mode)

    static Theme &instance();

    /// 从配置载入模式，并订阅系统配色变化。必须在建窗口之前调用。
    void load();

    Mode mode() const { return m_mode; }

    /// 当前实际生效的是不是深色。System 模式下取决于系统配色。
    bool isDark() const;

    /// 切换模式。persist=true 时写回配置；persist=false 只影响本次运行。
    /// 无论是否持久化，都会立刻重新应用主题。
    void setMode(Mode mode, bool persist = true);

    /// 把当前主题落地到 QPalette + qss + 所有控件的内联样式。
    /// 可重复调用：系统配色变化、用户切换模式都走这里。
    void apply();

    static QString modeToString(Mode mode);
    /// 解析配置值；无法识别时返回 System 并把 ok 置 false（调用方自行决定是否告警）。
    static Mode modeFromString(const QString &text, bool *ok = nullptr);

signals:
    /// 主题已落地（每次 apply 结束发一次）。给不依赖 qss、需要手工取色的地方用，
    /// 例如主窗口的投影（QGraphicsDropShadowEffect 的颜色不读样式表，得手动刷新）。
    void themeChanged();

private:
    explicit Theme(QObject *parent = nullptr);

    bool systemPrefersDark() const;
    /// qss 模板本身（带 @token），只从资源里读一次。
    QString styleTemplate();

    Mode m_mode = Mode::System;
    QString m_template;
};

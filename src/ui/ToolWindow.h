#pragma once

// ---------------------------------------------------------------------------
// 单个工具的独立窗口。
//
// 关键设计：
//   * 用**原生边框**（Qt::Window，不加 FramelessWindowHint）：独立窗口要参与
//     任务栏 / Alt+Tab / 贴边 / 最大化，自绘边框得把 startSystemMove + QSizeGrip
//     绝对定位再实现一遍，三平台风险远大于视觉收益。
//   * 不用 Qt::Tool：Windows 上不进任务栏、macOS 上应用失焦就藏起来，
//     用户会以为窗口丢了。
//   * 面板生命周期绑窗口：关窗即销毁面板（与主窗口「不缓存面板」的约定一致）。
//     同一工具可以同时有「内嵌面板」和「独立窗口面板」两份实例，插件不得假设单实例。
//   * 每个工具最多一个独立窗口，去重由 MainWindow 负责。
// ---------------------------------------------------------------------------

#include <QString>
#include <QWidget>

struct ToolEntry;

class ToolWindow : public QWidget
{
    Q_OBJECT

public:
    explicit ToolWindow(const ToolEntry &entry, QWidget *parent = nullptr);

    QString toolId() const { return m_toolId; }

    /// 前台化：已最小化先还原，再 raise + activate。重复打开走这里而不是新建窗口。
    void showAndRaise();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QString m_toolId;
    QWidget *m_root = nullptr;
    QWidget *m_panel = nullptr;   ///< 唯一存活的插件面板，随窗口析构一起销毁
};

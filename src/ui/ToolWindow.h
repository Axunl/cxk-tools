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
//   * 「从主窗口弹出来」时**接管**那个内嵌面板（构造参数 panel 非空），而不是再
//     createPanel() 一份：用户正在编辑的内容、光标、撤销栈、滚动位置都在那个 QWidget
//     实例里，重新创建等于把数据丢了。面板是纯 QWidget，跨顶层窗口 reparent 是合法的。
//   * 每个工具最多一个独立窗口，去重由 MainWindow 负责。
//   * 位置/大小按工具各记一份（Settings 的 "tool/<id>"）：用户摆过一次之后，每次弹出
//     都回到原处，不再重摆。只有从没弹过（或几何因换屏失效）时才给一个落点 ——
//     优先对齐主窗口当前位置，否则主屏居中。
// ---------------------------------------------------------------------------

#include <QRect>
#include <QString>
#include <QWidget>

struct ToolEntry;

class ToolWindow : public QWidget
{
    Q_OBJECT

public:
    /// @param entry      工具条目（取 id / 名称 / 图标）
    /// @param panel      宿主移交过来的现成面板；为空时自己 createPanel() 一份新的
    /// @param parent     父窗口，正常都是 nullptr（独立顶层窗口）
    /// @param anchorFrame 主窗口当前的 frame 几何。**仅在这个工具还没有历史位置时**
    ///                    用作落点：独立窗口对齐到主窗口左上角，避免首次弹出就跳到
    ///                    屏幕中央（主窗口随即隐藏，用户会觉得窗口凭空冒出来）。
    ///                    传空矩形则退回主屏居中。
    explicit ToolWindow(const ToolEntry &entry, QWidget *panel = nullptr, QWidget *parent = nullptr,
                        const QRect &anchorFrame = QRect());

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

#include "ToolWindow.h"

#include "Settings.h"
#include "ToolRegistry.h"
#include "WindowGeometry.h"

#include <QCloseEvent>
#include <QLabel>
#include <QVBoxLayout>

namespace {

constexpr int kDefaultWidth = 880;
constexpr int kDefaultHeight = 640;
constexpr int kMinWidth = 560;
constexpr int kMinHeight = 420;
/// 面板顶部留白：插件面板自己的 margins 是 (28, 0, 28, 20)，上方靠宿主页头提供空间。
/// 独立窗口没有页头，不加这一条会直接贴住系统标题栏。
constexpr int kTopPadding = 12;

} // namespace

ToolWindow::ToolWindow(const ToolEntry &entry, QWidget *panel, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_toolId(entry.info.id)
{
    // 关窗即销毁：哈希里的 QPointer 会自动变成空指针，不用手动摘表。
    setAttribute(Qt::WA_DeleteOnClose, true);
    // objectName 给 --shot 用：截图时要在顶层窗口里认出独立窗口（它不在主窗口子树里）
    setObjectName(QStringLiteral("toolWindow"));
    setWindowTitle(QStringLiteral("%1 · CXK 工具箱").arg(entry.name()));
    setWindowIcon(entry.icon);   // 任务栏/Alt+Tab 用工具自己的图标，多窗口时好认
    setMinimumSize(kMinWidth, kMinHeight);

    WindowGeometry::restore(this,
                            Settings::instance().windowGeometry(QStringLiteral("tool/") + m_toolId),
                            QSize(kDefaultWidth, kDefaultHeight));

    m_root = new QWidget(this);
    m_root->setObjectName(QStringLiteral("toolWindowRoot"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_root);

    auto *rootLayout = new QVBoxLayout(m_root);
    rootLayout->setContentsMargins(0, kTopPadding, 0, 0);
    rootLayout->setSpacing(0);

    // 优先接管宿主移交过来的面板（从主窗口「弹出来」的场景）：那个面板里已经有
    // 用户正在编辑的内容，重新 createPanel() 只会得到一张空面板。
    // 否则（磁贴右键 / 命令行 --popout）自己新建一份。
    m_panel = panel;
    if (!m_panel && entry.tool)
        m_panel = entry.tool->createPanel(nullptr);
    if (!m_panel) {
        // 插件违约（返回空）时给占位页，绝不崩溃
        auto *fallback = new QLabel(QStringLiteral("这个工具没有返回面板"), m_root);
        fallback->setAlignment(Qt::AlignCenter);
        m_panel = fallback;
    }
    // setParent 会把面板从原来的 QStackedWidget 里彻底摘出来（调用方已先 removeWidget），
    // 面板自身的布局、撤销栈、滚动位置都跟着走，不经过任何序列化。
    m_panel->setParent(m_root);
    rootLayout->addWidget(m_panel);

    // 接管来的面板是被 removeWidget() 摘下来的，摘的时候 Qt 已经调过 hide()：
    // 那个"隐藏"状态会一直记在控件的 WA_WState_Hidden 上，父窗口 show() 也带不出来，
    // 症状就是独立窗口一片空白。必须显式 show() 一次把标记清掉。
    m_panel->show();
}

void ToolWindow::showAndRaise()
{
    show();
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

void ToolWindow::closeEvent(QCloseEvent *event)
{
    // 面板是 m_root 的子对象，随窗口析构一起销毁 —— 这里不要再 delete 一次。
    Settings::instance().setWindowGeometry(QStringLiteral("tool/") + m_toolId, saveGeometry());
    QWidget::closeEvent(event);
}

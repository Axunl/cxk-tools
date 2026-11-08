#pragma once

// ---------------------------------------------------------------------------
// 取色器面板（插件内部类，宿主只认 ITool）。
//
// 交互：**打开工具即进入「吸管模式」** —— 面板首次显示时，把自己所属的顶层窗口
// （主窗口或独立窗口）收起来，改显示一个**跟随鼠标的小吸管浮窗**（放大镜 + 实时
// HEX 色值，无边框置顶、对鼠标透明、不抢焦点）。用户把光标移到要取的目标颜色上，
// **按下左键即锁定并自动把 HEX 复制到剪贴板**（「按下即取色+复制」，不是按住拖动），
// 随后吸管浮窗消失、取色动作整体结束（主窗口不再恢复显示）。
// 取色态用一个定时器轮询全局光标位置，实时移动浮窗并刷新放大镜与色值；裸 Esc 可取消。
// 这里刻意**不 grabMouse()**：grabMouse() 会吞掉其他应用的鼠标事件；
// 改用 QGuiApplication::mouseButtons() 读全局鼠标状态，浮窗只负责显示、不抢事件。
//
// 面板里的状态（当前颜色、历史、取色状态机）全部是实例成员：宿主不缓存面板，
// 同一工具可能同时存在「内嵌」与「独立窗口」两份，绝不能用静态变量串状态。
// ---------------------------------------------------------------------------

#include <QColor>
#include <QEvent>
#include <QPoint>
#include <QVector>
#include <QWidget>

class ColorChip;
class ColorSwatch;
class EyedropperWindow;
class LoupeWidget;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QVBoxLayout;

class ColorPickerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ColorPickerPanel(QWidget *parent = nullptr);

protected:
    /// 面板一显示即自动进入「吸管模式」：收起身后窗口、显示吸管浮窗、开始取色。
    /// 用户点开取色器就是要取色，没必要先看到静态面板再点按钮。延一轮事件循环再
    /// startPicking()，等宿主布局/绘制完成。自动取色只触发一次（m_autoPicked 挡门）。
    void showEvent(QShowEvent *event) override;

    /// 面板被页栈藏起来（回首页、进设置、收起窗口）时立即停止抓屏采样，
    /// 与内存监控插件的「不可见就不采样」一致，避免后台每 40ms 抓一次屏。
    /// 吸管模式里窗口是面板自己收起的，此时**不停**采样（见 m_picking 判断）。
    void hideEvent(QHideEvent *event) override;

    /// 取色态下的裸 Esc 只取消取色，不冒泡到窗口层（否则会直接退回首页）。
    void keyPressEvent(QKeyEvent *event) override;

private:
    /// 建左侧预览/操作栏与右侧格式栏，字段行数量由 colortext::formatFields 决定。
    void buildUi();

    /// 把颜色同步到色块、格式输入框与状态；record 决定是否写入历史，
    /// syncInput 决定是否覆盖手动输入框（取样实时预览时不能打断用户打字）。
    void applyColor(const QColor &color, bool record, bool syncInput);

    /// 把颜色放进「最近颜色」（最多 10 个，去重，新颜色排最前）。
    void recordColor(const QColor &color);
    void refreshChips();

    void startPicking();
    void stopPicking();
    void onPickTick();

    /// 进入/退出吸管模式：进入时隐藏本面板所属的顶层窗口（主窗口或独立窗口）并显示
    /// 跟随鼠标的吸管浮窗；退出（锁定复制后）关闭浮窗、整体结束取色动作。
    void showEyedropper();
    void hideEyedropper();

    /// 锁定当前颜色并复制：写历史、规范 HEX、自动复制，随后关闭吸管浮窗并结束。
    /// 从 onPickTick 的「按下左键即锁定」分支调用。
    void lockAndCopy();

    /// 光标是否落在**本应用**的某个可见顶层窗口里（含主窗口、独立工具窗口、
    /// 弹出菜单）。取色态下这些位置一律不采样，只保留当前颜色。
    static bool cursorOverOwnWindow(const QPoint &globalPos);

    void applyInput();
    void pasteFromClipboard();
    void copyText(const QString &text, const QString &label);
    void copyAllFields();

    /// error=true 会把状态文字标红（qss 的 #cpStatus[state="error"]），
    /// 动态属性改完必须重新 polish，否则样式不刷新。
    void setStatus(const QString &text, bool error = false);

    QColor  m_color;
    QVector<QColor> m_history;

    ColorSwatch  *m_swatch = nullptr;
    LoupeWidget  *m_loupe = nullptr;
    EyedropperWindow *m_eyedropper = nullptr;
    QLabel       *m_posLabel = nullptr;
    QLabel       *m_status = nullptr;
    QLabel       *m_hint = nullptr;
    QPushButton  *m_pickBtn = nullptr;
    QPushButton  *m_pasteBtn = nullptr;
    QPushButton  *m_copyAllBtn = nullptr;
    QPushButton  *m_applyBtn = nullptr;
    QPushButton  *m_clearHistoryBtn = nullptr;
    QLineEdit    *m_hexEdit = nullptr;
    QVector<QLineEdit *> m_fieldEdits;
    QVector<ColorChip *> m_chips;
    QTimer       *m_pickTimer = nullptr;

    /// 取色状态机：进入取色态时那次点击的左键仍按着则先忽略，
    /// 之后「按下左键」即锁定取色并复制（「按下即取色」语义）。
    bool m_picking = false;
    bool m_ignoreUntilRelease = false;

    /// 自动取色只触发一次。锁定后吸管浮窗已关闭、不再触发 showEvent；保留挡门以防
    /// 宿主在结束前又触发一次 showEvent 被误判成「再次打开取色器」。
    bool m_autoPicked = false;

    /// 吸管模式下，面板所属顶层窗口是面板自己收起的；hideEvent 此时不能停采样。
    bool m_eyedropperMode = false;

    /// 连续抓屏失败计数；成功一次即清零。见 kMaxGrabFailures。
    int m_grabFailures = 0;

    /// 取色 tick 重入锁。`QScreen::grabWindow()` / 剪贴板读写在 Windows 上会内部
    /// 转消息泵，导致 40ms 定时器在本 tick 未返回时又触发一次 `onPickTick`；嵌套 tick
    /// 会并发改写 `m_color` / `m_image` / `m_fieldEdits` 这些隐式共享对象的引用计数，
    /// 直接堆损坏（0xC0000374，实测表现为「打开取色器卡住→进程崩溃」）。置位后嵌套调用
    /// 直接返回；用 RAII 在函数出口复位，避免中途 return 漏清。
    bool m_inTick = false;
};

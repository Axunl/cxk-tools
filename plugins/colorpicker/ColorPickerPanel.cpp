#include "ColorPickerPanel.h"

#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用）

#include "ColorProbe.h"
#include "ColorText.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QFont>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>   // QKeyEvent / QHideEvent 的完整定义在 QtGui/qevent.h，不是 <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

// 全局十字光标只在 Windows 上用原生 API 实现；其余平台见 CrosshairCursor 的说明。
// 先定 WINVER/_WIN32_WINNT 再包含 windows.h：OCR_NORMAL 等光标常量在 WinUser.h 里被
// `#ifdef OEMRESOURCE` 保护，而 OEMRESOURCE 在 WIN32_LEAN_AND_MEAN 下不会自动定义，
// 不定的话 MSVC 拿不到这些常量，直接报未定义标识符。故这里显式补上 OEMRESOURCE。
#if defined(Q_OS_WIN)
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define OEMRESOURCE
#include <windows.h>
#endif

namespace {

/// 取色期间把**系统级**鼠标光标临时换成十字准星（+），取完恢复。
///
/// 为什么用原生 API 而不是 Qt 的 setOverrideCursor：Qt 的 override cursor 只在
/// **本应用自己的窗口**上生效，鼠标移到别的应用（取色目标往往是浏览器/设计稿/
/// 图片查看器）时光标会变回对方定义的箭头，用户就失去了「十字准星正对取样点」
/// 的定位感。Windows 的 SetSystemCursor 替换的是系统共享的 OCR_NORMAL 光标，
/// 跨应用生效；结束后必须恢复，否则整个系统鼠标都一直是十字。
///
/// 三平台策略：
///   - Windows：SetSystemCursor 替换 OCR_NORMAL；恢复时用 SPI_SETCURSORS 让系统按
///     注册表**整体重载**光标方案（不备份、不写回，原因见 resetSystemCursors）；
///   - macOS / Linux：无等价的「全局替换系统光标」的 Qt 公开接口，退而用
///     QGuiApplication::setOverrideCursor(Qt::CrossCursor)（仅本应用窗口内生效），
///     不静默假装成功——取色说明文案已提示用户「十字准星以放大镜中心为准」。
/// 其余平台同 macOS/Linux 兜底。
///
/// 幂等性：grab()/release() 用 s_active 挡门，重复调用不会叠加 override cursor，
/// 也不会对已经复位的光标再做一次系统调用。
class CrosshairCursor
{
public:
    /// 切到十字准星。幂等。
    static void grab()
    {
        if (s_active)
            return;
        s_active = true;
#if defined(Q_OS_WIN)
        // LoadCursor 返回的是系统共享光标句柄，不能直接交给 SetSystemCursor（后者会
        // destroy 传入句柄，破坏共享光标资源）。CopyImage 复制一份私有副本，flags=0
        // 表示「复制成功后返回副本、不删除原图」。
        HCURSOR cross = static_cast<HCURSOR>(
            CopyImage(LoadCursor(nullptr, IDC_CROSS), IMAGE_CURSOR, 0, 0, 0));
        if (cross)
            SetSystemCursor(cross, OCR_NORMAL);
#else
        // 非 Windows：Qt 层 override cursor，仅本应用窗口内有效（能力说明见类注释）。
        QGuiApplication::setOverrideCursor(QCursor(Qt::CrossCursor));
#endif
    }

    /// 复位系统光标（Windows）/ 弹出 override cursor（其余平台）。
    ///
    /// Windows 上**不按 s_active 挡门**：系统光标是跨进程持久的，上一次运行如果被
    /// 强杀（没走到本函数的正常路径），整个桌面的鼠标会一直是十字；这次运行只要调用
    /// 一次 SPI_SETCURSORS 就能顺手复位，属于廉价自愈。非 Windows 平台必须保留挡门，
    /// 否则会多弹一次 override cursor 栈，导致光标状态错乱。
    static void release()
    {
#if defined(Q_OS_WIN)
        s_active = false;
        resetSystemCursors();
#else
        if (!s_active)
            return;
        s_active = false;
        QGuiApplication::restoreOverrideCursor();
#endif
    }

#if defined(Q_OS_WIN)
    /// 把系统光标整体复位成「注册表里的当前光标方案」。
    ///
    /// **刻意不用「备份原光标句柄，结束时 SetSystemCursor 写回」那套。** 两个原因：
    ///   1. SetSystemCursor 改的是**系统级持久设置**，和进程生命周期无关。进程只要
    ///      没走到 release()（宿主关掉工具、退出应用、插件卸载、被杀进程），OCR_NORMAL
    ///      就一直是十字，整个桌面的鼠标都变成 +，直到重启或有人重置。
    ///   2. 一旦上一条发生过，下一次 grab() 里 `LoadCursor(nullptr, IDC_ARROW)` 取到的
    ///      「标准箭头」其实已经被换成十字，备份到的就是十字本身；release() 再写回去，
    ///      等于把十字写回系统 —— 于是「取完色鼠标永远是十字」。实测就是这个现象。
    ///
    /// SPI_SETCURSORS 让系统重新从注册表加载全部光标（用户自定义的光标方案也会原样
    /// 重载，不会被重置成默认箭头），是唯一能自愈、也不依赖进程是否正常退出的复位方式。
    static void resetSystemCursors()
    {
        // 失败也不上报：这是收尾动作，弹窗/日志刷屏比光标多留半秒更烦人。
        // 返回值只在 --shot 验证时用 qInfo 打印过，结论是恒为 1（成功）。
        ::SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    }
#endif

private:
    static inline bool s_active = false;
};

/// 实时查询「左键此刻是否按下」。
/// 不能用 QGuiApplication::mouseButtons()：它不是实时查询，只反映 Qt 应用**最近一次
/// 收到的鼠标事件**里的按键状态。吸管模式下宿主窗口已隐藏、浮窗对鼠标透明，光标悬停在
/// 其他应用上时本应用根本收不到鼠标事件，mouseButtons() 永远停在「未按下」——
/// 表现就是「预览跟着光标走，但按下左键死活不锁定」。
/// Windows 上用 GetAsyncKeyState 查物理按键的实时状态（全局、不需要焦点）；
/// 其他平台没有等价的 Qt API，退回 mouseButtons() 兜底（macOS/Linux 下该问题同样存在，
/// 属已知边界，实机再验证）。
inline bool leftButtonDownNow()
{
#if defined(Q_OS_WIN)
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
    return (QGuiApplication::mouseButtons() & Qt::LeftButton) != 0;
#endif
}

/// 轮询间隔。40ms 足够跟上鼠标移动，又不会把 grabWindow 打得太密；
/// 只在取色态运行，面板隐藏或退出取色态立刻停（见 hideEvent / stopPicking）。
constexpr int kPickIntervalMs = 40;

/// 「最近颜色」上限。宿主不缓存面板，历史只活在这一份面板里，10 个够用且不占位。
constexpr int kHistoryMax = 10;

/// 连续抓屏失败多少次才认定能力不可用。切屏、锁屏、远程桌面重连时
/// grabWindow 可能只抖一下，自动取色不该因为一次瞬时失败就停下要用户重点按钮。
/// 3 次 ≈ 120ms，之后照样把明确原因报出来（规则 3：不允许静默失败）。
constexpr int kMaxGrabFailures = 3;

/// 历史色块边长（逻辑像素）。
constexpr int kChipSize = 26;

/// 半透明颜色的棋盘格底。因为类都自绘，画在这里共用，避免拼内联 qss。
void paintCheckerboard(QPainter *p, const QRect &rect, int cell)
{
    static const QColor light(QStringLiteral("#FFFFFF"));
    static const QColor shade(QStringLiteral("#CBD5E1"));
    p->fillRect(rect, light);
    for (int y = rect.top(); y < rect.bottom(); y += cell) {
        for (int x = rect.left(); x < rect.right(); x += cell) {
            if ((((x - rect.left()) / cell) + ((y - rect.top()) / cell)) % 2 == 0)
                p->fillRect(x, y, cell, cell, shade);
        }
    }
}

/// 动态属性（state / accent）改完后必须重新 polish，否则 qss 不会重算样式。
/// 与内存监控插件里同名的辅助函数一个道理。
void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

/// 取色 tick 的重入锁（RAII）：构造置位、析构复位，函数中途任何 return 都不会漏清。
/// 见 ColorPickerPanel::m_inTick 的注释 —— 防止 grabWindow/剪贴板内部转消息泵时
/// 定时器嵌套触发 onPickTick，并发改写隐式共享对象的引用计数导致堆损坏。
struct TickGuard
{
    bool &flag;
    explicit TickGuard(bool &f) : flag(f) { flag = true; }
    ~TickGuard() { flag = false; }
};

} // namespace

// 下面三个自绘控件定义在**全局命名空间**：ColorPickerPanel.h 里有同名的前向
// 声明，若放进匿名命名空间，编译器会看到两个同名类型（MSVC C2872），
// 头文件里的成员指针也会退化成「未定义类型」（C2027）。

///
/// 大色块预览：圆角 + 棋盘格透明底 + 色值大字 + 建议文字色/对比度。
/// 自绘而不是 setStyleSheet，是因为颜色每次取色都变，拼样式串既慢又难维护。
/// 不带 Q_OBJECT：只重写 paintEvent，不需要自己的信号槽。
///
class ColorSwatch : public QWidget
{
public:
    explicit ColorSwatch(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        // 固定高度：放进带 stretch 的布局时若让它自由伸展，会把左栏其余控件
        // 全挤到窗口底部（首版截图里色块占掉 260px），视觉重心严重偏上。
        setFixedHeight(150);
        setObjectName(QStringLiteral("cpSwatch"));
    }

    void setColor(const QColor &color)
    {
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRect r = rect().adjusted(0, 0, -1, -1);
        const QRectF rf(r);

        // 先用圆角路径裁剪，再画棋盘格与颜色，四角才是圆的。
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(rf, 10, 10);
        p.setClipPath(clip);
        paintCheckerboard(&p, r, 8);
        p.fillRect(r, m_color);
        p.restore();
        p.setPen(QPen(xtools::theme::color(xtools::theme::Color::Border), 1));
        p.drawRoundedRect(rf, 10, 10);

        // 文字颜色按「合成到白底后的实际观感」选黑白，保证深色/浅色底都读得清。
        const QColor text = colortext::suggestedText(m_color);
        p.setPen(text);
        QFont big = p.font();
        big.setPointSize(20);
        big.setBold(true);
        p.setFont(big);
        p.drawText(r.adjusted(18, 14, -18, -14), Qt::AlignLeft | Qt::AlignTop,
                   colortext::formatHex(m_color));

        QFont small = p.font();
        small.setPointSize(9);
        small.setBold(false);
        p.setFont(small);
        const QString caption = QStringLiteral("建议文字色 %1 · 对比度 %2:1")
            .arg(colortext::formatHex(text),
                 QString::number(colortext::contrastRatio(m_color, text), 'f', 1));
        p.drawText(r.adjusted(18, 14, -18, -14), Qt::AlignLeft | Qt::AlignBottom, caption);
    }

private:
    QColor m_color;
};

///
/// 放大镜：把抓到的屏幕邻域原图按像素放大，中心画十字标出取样点。
/// 抓图失败/手动输入时退化成一块纯色，不显示空白控件。
///
class LoupeWidget : public QWidget
{
public:
    explicit LoupeWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(108, 108);
        setObjectName(QStringLiteral("cpLoupe"));
    }

    void setSample(const QImage &image, const QColor &color)
    {
        m_image = image;
        m_color = color;
        update();
    }

    void setSolid(const QColor &color)
    {
        m_image = QImage();
        m_color = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = rect().adjusted(0, 0, -1, -1);
        if (m_image.isNull()) {
            p.fillRect(r, m_color);
        } else {
            p.fillRect(r, xtools::theme::color(xtools::theme::Color::BgSubtle));
            // 关掉平滑：放大镜要看到一个个像素方块，插值会把边界糊掉。
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            const QImage scaled = m_image.scaled(r.size(), Qt::KeepAspectRatio,
                                                 Qt::FastTransformation);
            QRect imageRect(0, 0, scaled.width(), scaled.height());
            imageRect.moveCenter(r.center());
            p.drawImage(imageRect.topLeft(), scaled);
        }

        // 中心十字画两遍（粗白 + 细深），任何底色下都不会「消失」。
        const QPoint c = r.center();
        for (int pass = 0; pass < 2; ++pass) {
            const QColor penColor = pass == 0 ? QColor(255, 255, 255, 230)
                                              : QColor(15, 23, 42, 230);
            p.setPen(QPen(penColor, pass == 0 ? 3 : 1));
            p.drawLine(c.x() - 9, c.y(), c.x() + 9, c.y());
            p.drawLine(c.x(), c.y() - 9, c.x(), c.y() + 9);
        }
        p.setPen(QPen(xtools::theme::color(xtools::theme::Color::Border), 1));
        p.drawRect(r);
    }

private:
    QImage m_image;
    QColor m_color;
};

///
/// 吸管浮窗：跟随鼠标的小窗口，内部是放大镜（LoupeWidget）+ 一行实时 HEX 色值。
///
/// 它是一个**独立顶层窗口**，而不是面板的子控件 —— 只有这样才能脱离宿主主窗口、
/// 跟随光标移动到屏幕任意位置取色。关键属性：
///   - Qt::FramelessWindowHint + Qt::Tool + Qt::WindowStaysOnTopHint：无边框、置顶，
///     不占任务栏，看起来就是「悬浮在屏幕上的吸管」；
///   - Qt::WindowDoesNotAcceptFocus + WA_ShowWithoutActivating：显示时不抢键盘/鼠标焦点，
///     否则一 show() 就把前台应用挤下去，用户没法继续点别的窗口；
///   - WA_TransparentForMouseEvents：对鼠标事件完全透明，浮窗出现在光标附近/下方时
///     也不会「吃掉」那次左键点击 —— 取色靠面板用 QGuiApplication::mouseButtons()
///     读全局鼠标状态完成，浮窗本身不处理任何鼠标事件。
/// 无 Q_OBJECT：只被动接收面板塞进来的取样数据重绘，不需要自己的信号槽。
///
class EyedropperWindow : public QWidget
{
public:
    explicit EyedropperWindow(QWidget *parent = nullptr)
        : QWidget(parent,
                 Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint
                     | Qt::WindowDoesNotAcceptFocus)
    {
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        // 半透明白底 + 细边，既能看清放大镜，又不会完全盖住下面的屏幕内容。
        setObjectName(QStringLiteral("cpEyedropper"));
        setAttribute(Qt::WA_StyledBackground, true);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        m_loupe = new LoupeWidget(this);
        layout->addWidget(m_loupe, 0, Qt::AlignHCenter);

        m_hex = new QLabel(this);
        m_hex->setObjectName(QStringLiteral("cpEyedropperHex"));
        m_hex->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_hex);
    }

    /// 更新放大镜内容与色值文字，并把窗口移动到光标附近（偏移一段距离，避免挡住取样点）。
    void updateSample(const QImage &around, const QColor &color, const QPoint &cursorGlobal)
    {
        m_loupe->setSample(around, color);
        m_hex->setText(colortext::formatHex(color));

        // 浮窗放在光标右下方，既贴近光标又能看清放大镜内容；越界时翻转到左上。
        // 偏移 16px 让中心取样点（十字）不被浮窗盖住。
        const QSize sz = sizeHint();
        QPoint pos = cursorGlobal + QPoint(16, 16);
        if (QScreen *screen = QGuiApplication::screenAt(cursorGlobal)) {
            const QRect avail = screen->availableGeometry();
            if (pos.x() + sz.width() > avail.right())
                pos.setX(cursorGlobal.x() - sz.width() - 16);
            if (pos.y() + sz.height() > avail.bottom())
                pos.setY(cursorGlobal.y() - sz.height() - 16);
        }
        move(pos);
    }

    /// 抓屏失败 / 锁定瞬间：退化成纯色放大镜。
    void updateSolid(const QColor &color) { m_loupe->setSolid(color); }

    /// 取色完成后的结果提示：把色值文字换成「已复制 #XXXXXX」，放大镜显示锁定色，
    /// 停留片刻由面板用 QTimer 延时后关闭浮窗。给用户明确的「复制了什么」反馈。
    void showResult(const QString &hex, const QColor &color)
    {
        m_loupe->setSolid(color);
        m_hex->setText(QStringLiteral("已复制 %1").arg(hex));
    }

private:
    LoupeWidget *m_loupe;
    QLabel *m_hex;
};

///
/// 历史色块。继承 QAbstractButton 只为复用 clicked 信号和 hover 状态，
/// 外观全部自绘；不用 qss，因为每个色块颜色都不同。
///
class ColorChip : public QAbstractButton
{
public:
    explicit ColorChip(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setFixedSize(kChipSize, kChipSize);
        setAttribute(Qt::WA_Hover, true);   // 没这个属性就收不到 HoverEnter/Leave
        setCursor(Qt::PointingHandCursor);
        hide();                             // 没历史时不占位
    }

    void setColor(const QColor &color)
    {
        m_color = color;
        update();
    }

    QColor color() const { return m_color; }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)
            update();   // underMouse() 变化后重画边框
        return QAbstractButton::event(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRect r = rect().adjusted(2, 2, -2, -2);
        const QRectF rf(r);

        p.save();
        QPainterPath clip;
        clip.addRoundedRect(rf, 6, 6);
        p.setClipPath(clip);
        paintCheckerboard(&p, r, 5);
        p.fillRect(r, m_color);
        p.restore();

        // hover 用主色加粗描边，静态属性下 hover 不可靠，这里靠 WA_Hover + 重画。
        p.setPen(underMouse() ? QPen(xtools::theme::color(xtools::theme::Color::Accent), 2)
                              : QPen(xtools::theme::color(xtools::theme::Color::Border), 1));
        p.drawRoundedRect(rf, 6, 6);
    }

private:
    QColor m_color;
};

// ---------------------------------------------------------------- 面板构造

ColorPickerPanel::ColorPickerPanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();

    m_pickTimer = new QTimer(this);
    m_pickTimer->setInterval(kPickIntervalMs);
    connect(m_pickTimer, &QTimer::timeout, this, &ColorPickerPanel::onPickTick);

    // 打开工具即进入「吸管模式」（showEvent 会延一轮再 showEyedropper），这里先给个
    // 过渡文案；即使 showEvent 还没触发，界面也不会停在空白。
    setStatus(QStringLiteral("正在进入屏幕取色；把光标移到要取的目标颜色上，按下左键即锁定并自动复制。"));
    // 给一个项目主色作为初始值，界面打开就不是空白，也顺手展示各格式的样子。
    applyColor(QColor(79, 107, 246), true, true);
    m_loupe->setSolid(m_color);

    // 退出应用时的最后一道保险：面板析构不一定发生在「窗口关闭」之后（宿主可能在
    // 进程退出流程里批量删控件，顺序不保证），而系统光标是进程级持久的。
    // 绑 this 为 context，面板先销毁则连接自动断开，不会碰到悬空对象。
    connect(qApp, &QCoreApplication::aboutToQuit, this, [] { CrosshairCursor::release(); });

    // 再补一道自愈：上一次运行若被强杀（taskkill / 宿主崩溃），OCR_NORMAL 可能还是
    // 十字，且那种残留不会随进程退出而消失。这里在打开工具时就复位一次，用户不必
    // 等到「取一次色」才恢复正常鼠标。
    CrosshairCursor::release();
}

ColorPickerPanel::~ColorPickerPanel()
{
    // 关工具 / 关窗口 / 插件卸载：不管走到哪条路径，系统光标都必须复位。
    // release() 幂等，正常取完色的场景这里是一次空操作。
    CrosshairCursor::release();

    // 吸管浮窗是没有父对象的独立顶层窗口（脱离宿主窗口树才能跟到屏幕任意位置），
    // 不手动 delete 会泄漏，且它可能仍显示在屏幕上。
    delete m_eyedropper;
    m_eyedropper = nullptr;
}

void ColorPickerPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 0, 28, 20);   // 与宿主页头 28px 留白对齐
    root->setSpacing(12);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(14);
    root->addLayout(columns, 1);

    // ------------------------------------------------------------ 左：预览与操作
    auto *left = new QVBoxLayout;
    left->setSpacing(10);
    columns->addLayout(left, 3);

    m_swatch = new ColorSwatch(this);
    left->addWidget(m_swatch, 0);   // 高度固定，剩余空间交给末尾的 stretch

    auto *loupeRow = new QHBoxLayout;
    loupeRow->setSpacing(12);
    m_loupe = new LoupeWidget(this);
    loupeRow->addWidget(m_loupe, 0, Qt::AlignTop);

    auto *loupeText = new QVBoxLayout;
    loupeText->setSpacing(6);
    m_posLabel = new QLabel(QStringLiteral("光标 —"), this);
    m_posLabel->setObjectName(QStringLiteral("cpCaption"));
    auto *loupeHint = new QLabel(
        QStringLiteral("打开工具即自动取色：放大镜中心的十字就是取样像素，移动鼠标实时预览，"
                       "把光标移到目标颜色上按下左键即锁定并自动复制 HEX；再点一次按钮或按 Esc 停止取色。"
                       "光标在本窗口内不取样，请移到要取色的目标颜色上。"), this);
    loupeHint->setObjectName(QStringLiteral("cpHint"));
    loupeHint->setWordWrap(true);
    loupeText->addWidget(m_posLabel);
    loupeText->addWidget(loupeHint);
    loupeText->addStretch(1);
    loupeRow->addLayout(loupeText, 1);
    left->addLayout(loupeRow);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_pickBtn = new QPushButton(QStringLiteral("屏幕取色（点击开始）"), this);
    m_pickBtn->setCheckable(true);
    m_pickBtn->setProperty("accent", true);
    // 打开工具已自动进入吸管模式，按钮此刻是 checked；再点一次（checked=false）就是停止，
    // 点与再点之间用户没手动开过取色，所以按钮语义始终是「停止 / 重新开始」而非反转。
    // 手动「重新开始」同样走吸管模式（showEyedropper + startPicking），与首次打开一致。
    connect(m_pickBtn, &QPushButton::clicked, this, [this](bool checked) {
        if (checked) {
            showEyedropper();
            startPicking();
        } else {
            stopPicking();
            hideEyedropper();
            // 用户主动停止要给明确反馈；这是新动作，覆盖掉之前的取样错误文案是合理的。
            setStatus(QStringLiteral("已停止取色；再点一次按钮重新开始，或直接输入颜色值后按回车。"));
        }
    });
    m_pasteBtn = new QPushButton(QStringLiteral("从剪贴板读入"), this);
    connect(m_pasteBtn, &QPushButton::clicked, this, &ColorPickerPanel::pasteFromClipboard);
    m_copyAllBtn = new QPushButton(QStringLiteral("复制全部格式"), this);
    connect(m_copyAllBtn, &QPushButton::clicked, this, &ColorPickerPanel::copyAllFields);
    actions->addWidget(m_pickBtn);
    actions->addWidget(m_pasteBtn);
    actions->addWidget(m_copyAllBtn);
    actions->addStretch(1);
    left->addLayout(actions);

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(8);
    auto *inputLabel = new QLabel(QStringLiteral("颜色值"), this);
    inputLabel->setObjectName(QStringLiteral("cpCaption"));
    m_hexEdit = new QLineEdit(this);
    m_hexEdit->setPlaceholderText(
        QStringLiteral("#RRGGBB / rgb(...) / hsl(...) / 颜色名，回车应用"));
    m_hexEdit->setClearButtonEnabled(true);
    m_applyBtn = new QPushButton(QStringLiteral("应用"), this);
    connect(m_applyBtn, &QPushButton::clicked, this, &ColorPickerPanel::applyInput);
    connect(m_hexEdit, &QLineEdit::returnPressed, this, &ColorPickerPanel::applyInput);
    inputRow->addWidget(inputLabel);
    inputRow->addWidget(m_hexEdit, 1);
    inputRow->addWidget(m_applyBtn);
    left->addLayout(inputRow);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("cpStatus"));
    m_status->setWordWrap(true);
    left->addWidget(m_status);

    auto *historyRow = new QHBoxLayout;
    historyRow->setSpacing(5);
    auto *historyLabel = new QLabel(QStringLiteral("最近颜色"), this);
    historyLabel->setObjectName(QStringLiteral("cpCaption"));
    historyRow->addWidget(historyLabel);
    for (int i = 0; i < kHistoryMax; ++i) {
        auto *chip = new ColorChip(this);
        connect(chip, &QAbstractButton::clicked, this, [this, chip] {
            if (!chip->color().isValid())
                return;
            applyColor(chip->color(), false, true);
            m_loupe->setSolid(m_color);   // 手动恢复的颜色没有屏幕邻域可放大
            setStatus(QStringLiteral("已恢复历史颜色 %1。").arg(colortext::formatHex(m_color)));
        });
        m_chips.append(chip);
        historyRow->addWidget(chip);
    }
    m_clearHistoryBtn = new QPushButton(QStringLiteral("清空"), this);
    m_clearHistoryBtn->setObjectName(QStringLiteral("cpSmallBtn"));
    m_clearHistoryBtn->setFixedWidth(48);
    connect(m_clearHistoryBtn, &QPushButton::clicked, this, [this] {
        m_history.clear();
        refreshChips();
        setStatus(QStringLiteral("已清空最近颜色。"));
    });
    historyRow->addWidget(m_clearHistoryBtn);
    historyRow->addStretch(1);
    left->addLayout(historyRow);

    // 平台限制常驻显示：macOS 未授权时系统只返回黑屏、Qt 不报错，
    // 不提前说明用户会以为吸管坏了（规则 3：不允许静默失败）。
    m_hint = new QLabel(QStringLiteral("屏幕取色说明：") + colorprobe::platformHint(), this);
    m_hint->setObjectName(QStringLiteral("cpHint"));
    m_hint->setWordWrap(true);
    left->addWidget(m_hint);
    left->addStretch(1);   // 内容靠上排，窗口变高时下方留白而不是拉长某个控件

    // ------------------------------------------------------------ 右：格式与复制
    auto *card = new QWidget(this);
    card->setObjectName(QStringLiteral("cpCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);   // QWidget 子类要显式开才画 qss 背景
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 14, 16, 14);
    cardLayout->setSpacing(10);

    auto *cardTitle = new QLabel(QStringLiteral("格式与复制"), card);
    cardTitle->setObjectName(QStringLiteral("cpCardTitle"));
    cardLayout->addWidget(cardTitle);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    // 行数由纯逻辑层的 formatFields() 决定，以后加格式不用改布局代码。
    const QList<colortext::Field> fields = colortext::formatFields(QColor(79, 107, 246));
    for (int i = 0; i < fields.size(); ++i) {
        auto *name = new QLabel(fields.at(i).label, card);
        name->setObjectName(QStringLiteral("cpFieldName"));
        name->setFixedWidth(46);

        auto *value = new QLineEdit(card);
        value->setObjectName(QStringLiteral("cpFieldValue"));
        value->setReadOnly(true);
        value->setProperty("cpLabel", fields.at(i).label);

        auto *copy = new QPushButton(QStringLiteral("复制"), card);
        copy->setObjectName(QStringLiteral("cpCopyBtn"));
        copy->setFixedWidth(46);
        // 捕获 QLineEdit 指针而不是当时的文本：颜色变了按钮复制的也是最新值。
        connect(copy, &QPushButton::clicked, this, [this, value] {
            copyText(value->text(), value->property("cpLabel").toString());
        });

        grid->addWidget(name, i, 0);
        grid->addWidget(value, i, 1);
        grid->addWidget(copy, i, 2);
        m_fieldEdits.append(value);
    }
    cardLayout->addLayout(grid);

    // 卡片下半不放空：把「怎么用」和 CMYK 的口径写清楚，避免用户把近似换算
    // 当成印刷分色值，也少一轮「为什么 hsl 和 rgb 对不上」的困惑。
    auto *cardHint = new QLabel(
        QStringLiteral("· 打开工具即自动取色，把光标移到目标颜色上按下左键即锁定并自动复制 HEX；\n"
                       "  再点一次按钮或按 Esc 停止。光标在本窗口内不取样。\n"
                       "· 也可直接输入 #RRGGBB、rgb()、hsl()、裸数字组或颜色名，回车应用。\n"
                       "· CMYK 是屏幕 RGB 的近似换算，不是印刷分色值。"), card);
    cardHint->setObjectName(QStringLiteral("cpHint"));
    cardHint->setWordWrap(true);
    cardLayout->addSpacing(6);
    cardLayout->addWidget(cardHint);
    cardLayout->addStretch(1);
    // 顶部对齐：卡片高度由内容决定，不跟着左栏被拉成一整块白色空白。
    columns->addWidget(card, 2, Qt::AlignTop);
}

// ---------------------------------------------------------------- 颜色同步

void ColorPickerPanel::applyColor(const QColor &color, bool record, bool syncInput)
{
    // 统一到 RGB 规格：屏幕像素、fromHslF、命名色混在一起时，
    // QColor 的相等比较与格式换算才不会因规格不同而给出不一致的结果。
    m_color = color.toRgb();
    m_swatch->setColor(m_color);

    const QList<colortext::Field> fields = colortext::formatFields(m_color);
    for (int i = 0; i < m_fieldEdits.size() && i < fields.size(); ++i)
        m_fieldEdits.at(i)->setText(fields.at(i).text);

    // syncInput=false 用于「实时预览」和「从剪贴板读入」：前者若每 40ms 覆盖
    // 输入框会打断打字，后者要保留用户粘贴的原始写法。预览态下由 onPickTick
    // 在输入框无焦点时单独同步，保证三处显示的颜色一致。
    if (syncInput)
        m_hexEdit->setText(colortext::formatHex(m_color));

    if (record)
        recordColor(m_color);
}

void ColorPickerPanel::recordColor(const QColor &color)
{
    // 与队首相同就不重复记账；顺带把后面重复的同色删掉，历史里每个颜色只出现一次。
    if (!m_history.isEmpty() && m_history.first() == color) {
        refreshChips();
        return;
    }
    for (int i = m_history.size() - 1; i >= 0; --i) {
        if (m_history.at(i) == color)
            m_history.removeAt(i);
    }
    m_history.prepend(color);
    if (m_history.size() > kHistoryMax)
        m_history.resize(kHistoryMax);
    refreshChips();
}

void ColorPickerPanel::refreshChips()
{
    for (int i = 0; i < m_chips.size(); ++i) {
        ColorChip *chip = m_chips.at(i);
        if (i < m_history.size()) {
            chip->setColor(m_history.at(i));
            chip->setToolTip(colortext::formatHex(m_history.at(i)));
            chip->show();
        } else {
            chip->hide();
        }
    }
    m_clearHistoryBtn->setEnabled(!m_history.isEmpty());
}

// ---------------------------------------------------------------- 取色状态机

void ColorPickerPanel::startPicking()
{
    if (m_picking)
        return;
    m_picking = true;
    m_ignoreUntilRelease = true;
    m_grabFailures = 0;
    m_pickBtn->setChecked(true);
    m_pickBtn->setText(QStringLiteral("取色中…"));
    // 一条写全：「怎么锁定」「怎么取消」都在状态栏里，取色态下 tick 不再覆盖状态
    // 文字，取样失败的红色错误文案才留得住。
    setStatus(QStringLiteral("取色中：把光标移到目标颜色上，移动实时预览，"
                             "按下左键即锁定并自动复制 HEX；按 Esc 取消。"));
    m_pickTimer->start();
}

void ColorPickerPanel::stopPicking()
{
    if (!m_picking)
        return;
    m_picking = false;
    m_pickTimer->stop();
    m_pickBtn->setChecked(false);
    m_pickBtn->setText(QStringLiteral("屏幕取色（点击开始）"));
    // 刻意不在这里改状态文字：取样失败时 error 文案要留在界面上给用户看。
}

void ColorPickerPanel::showEyedropper()
{
    // 进入吸管模式：隐藏本面板所属的顶层窗口（主窗口或独立窗口），改显示跟随鼠标的
    // 吸管浮窗。隐藏后 1) 宿主窗口不再遮挡要取的目标；2) cursorOverOwnWindow() 因窗口
    // 不可见而不再把光标位置判定为「自己窗口内」，全屏任意位置都能取色。隐藏会触发
    // 本面板的 hideEvent，但 m_eyedropperMode 已置位，hideEvent 不会停下采样。
    m_eyedropperMode = true;

    // 吸管浮窗惰性创建（首次进入时 new），窗口无父对象、随面板析构一起销毁。
    if (!m_eyedropper)
        m_eyedropper = new EyedropperWindow();
    m_eyedropper->updateSolid(m_color);
    // 先定位到当前光标再 show，避免浮窗在屏幕左上角闪一下。
    m_eyedropper->move(QCursor::pos() + QPoint(16, 16));
    m_eyedropper->show();

    // 取色期间把系统鼠标光标换成十字准星（+），结束/取消时在 hideEyedropper 恢复。
    CrosshairCursor::grab();

    if (QWidget *top = window())
        top->hide();
}

void ColorPickerPanel::hideEyedropper()
{
    // 结束吸管模式：关浮窗、恢复鼠标光标、整体收起（不恢复宿主窗口——用户要的是
    // 「取完就结束」）。
    CrosshairCursor::release();
    if (m_eyedropper)
        m_eyedropper->hide();
    m_eyedropperMode = false;
    // 取完色直接结束：把本面板所属顶层窗口收进托盘/关闭，不再弹回取色器面板。
    if (QWidget *top = window())
        top->hide();
}

void ColorPickerPanel::onPickTick()
{
    // 重入保护：QScreen::grabWindow() / 剪贴板在 Windows 上会内部转消息泵，使 40ms
    // 定时器在本 tick 尚未返回时又触发一次 onPickTick；嵌套 tick 会并发改写 m_color /
    // m_image / m_fieldEdits 等隐式共享对象的引用计数 → 堆损坏（0xC0000374，
    // 实测「打开取色器卡住→进程崩溃」）。置位后嵌套调用直接返回。
    if (m_inTick)
        return;
    TickGuard tickGuard(m_inTick);

    // 左键状态必须实时查询（见 leftButtonDownNow 的注释），这是不 grabMouse() 的
    // 前提 —— 不抢别的应用的鼠标事件也能完成取色。
    const bool leftDown = leftButtonDownNow();

    // 触发按钮的那次点击可能还没完全释放；先把这次按下忽略掉，
    // 否则一进入取色态就被误判成「用户按了一下左键」而立刻锁定。
    if (m_ignoreUntilRelease) {
        if (leftDown)
            return;
        m_ignoreUntilRelease = false;
    }

    const QPoint cursorPos = QCursor::pos();

    // 光标还在本应用窗口内时不采样。吸管模式下宿主窗口已隐藏、吸管浮窗对鼠标透明，
    // 不会命中此分支；这里兜底覆盖「手动点按钮取色」的普通路径——点开工具时光标通常
    // 就在磁贴/面板上，抓到的是自己的背景色；更糟的是用户点输入框或「复制」的那次按下
    // 会被当成锁定动作，把没取到的颜色写进历史。这里直接跳过整个 tick。
    if (cursorOverOwnWindow(cursorPos)) {
        const QString caption = QStringLiteral("光标在本窗口内 —");
        if (m_posLabel->text() != caption)
            m_posLabel->setText(caption);
        return;
    }

    const colorprobe::Sample sample = colorprobe::grabAt(cursorPos);
    if (!sample.ok) {
        if (++m_grabFailures < kMaxGrabFailures)
            return;   // 瞬时失败：不打断取色，也不把界面刷成红色
        setStatus(sample.error, true);
        stopPicking();
        hideEyedropper();
        return;
    }
    m_grabFailures = 0;

    m_posLabel->setText(QStringLiteral("光标 (%1, %2)").arg(sample.pos.x()).arg(sample.pos.y()));
    applyColor(sample.color, false, false);
    // 实时预览时输入框若停在旧色，会与色块/格式表对不上。
    // 只在输入框没有焦点时同步：用户正在里面打字（包括粘贴了原始写法）就不打断。
    if (!m_hexEdit->hasFocus())
        m_hexEdit->setText(colortext::formatHex(m_color));
    m_loupe->setSample(sample.around, sample.color);

    // 吸管模式：把取到的像素与色值实时喂给跟随鼠标的浮窗。
    if (m_eyedropperMode && m_eyedropper && m_eyedropper->isVisible())
        m_eyedropper->updateSample(sample.around, sample.color, cursorPos);

    // 「按下即取色」：左键按下那一下就锁定当前颜色并复制，而不是按住再松开。
    // 吸管模式下用户把光标移到目标颜色上点一下即可完成取色。
    if (leftDown)
        lockAndCopy();
}

void ColorPickerPanel::lockAndCopy()
{
    // 锁定：写历史、把输入框规范成 HEX、停止取色、自动复制 HEX，最后在浮窗上提示
    // 「已复制 #XXXXXX」停留片刻，再关闭浮窗、整体结束取色动作（不恢复宿主窗口）。
    stopPicking();
    recordColor(m_color);
    const QString hex = colortext::formatHex(m_color);
    m_hexEdit->setText(hex);
    // 「按下左键即复制取色代码」：锁定后把 HEX 写进剪贴板。刻意放到下一轮事件循环
    // 而不是在本 tick 里同步做 —— 剪贴板 setText/回读在 Windows 上会转消息泵，
    // 在定时器回调里做要么被重入锁挡住、要么在面板被销毁时 this 悬空。绑定 this
    // 为 context，面板销毁则自动取消这次复制，不会写到剪贴板。
    QTimer::singleShot(0, this, [this, hex] {
        QClipboard *clipboard = QGuiApplication::clipboard();
        QString resultText;
        bool error = false;
        if (!clipboard) {
            resultText = QStringLiteral("已锁定 %1，剪贴板不可用").arg(hex);
            error = true;
        } else {
            clipboard->setText(hex);
            // QClipboard::setText() 无返回值，回读一次确认真的写进去了（规则 3：不静默失败）。
            if (clipboard->text() != hex) {
                resultText = QStringLiteral("复制 %1 失败，请手动复制").arg(hex);
                error = true;
            } else {
                resultText = QStringLiteral("已复制 %1").arg(hex);
            }
        }

        // 结果反馈：在吸管浮窗上显示「已复制 #XXXXXX」（或失败原因），停留约 1.2 秒让
        // 用户看清复制了什么，再关闭浮窗、整体结束。浮窗仍保持跟随刚才的取样位置。
        if (m_eyedropper && m_eyedropper->isVisible()) {
            m_eyedropper->showResult(resultText, m_color);
            // 延时后收尾。绑 this 为 context：面板销毁则自动取消，不会误关别的窗口。
            QTimer::singleShot(1200, this, [this] { hideEyedropper(); });
        } else {
            // 浮窗已不在（异常/手动停止），直接结束，但仍在面板状态栏留一句反馈。
            setStatus(resultText, error);
            hideEyedropper();
        }
    });
}

bool ColorPickerPanel::cursorOverOwnWindow(const QPoint &globalPos)
{
    // topLevelWidgets() 覆盖主窗口、独立工具窗口和弹出菜单，都是「自己的」窗口。
    // frameGeometry() 是全局逻辑坐标且含边框（无边框窗口与 geometry() 相同），
    // 与 QCursor::pos() 同一坐标系；最小化的窗口不算挡住屏幕。
    const auto windows = QApplication::topLevelWidgets();
    for (QWidget *window : windows) {
        if (!window || !window->isVisible() || window->isMinimized())
            continue;
        if (window->frameGeometry().contains(globalPos))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- 输入 / 复制

void ColorPickerPanel::applyInput()
{
    const QString text = m_hexEdit->text();
    const QColor color = colortext::parseColor(text);
    if (!color.isValid()) {
        setStatus(QStringLiteral("无法识别颜色值：%1").arg(text.trimmed().left(60)), true);
        return;
    }
    stopPicking();
    applyColor(color, true, true);
    m_loupe->setSolid(m_color);
    setStatus(QStringLiteral("已应用 %1。").arg(colortext::formatHex(m_color)));
}

void ColorPickerPanel::pasteFromClipboard()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        setStatus(QStringLiteral("剪贴板不可用。"), true);
        return;
    }
    // 剪贴板可能是整段代码/日志：只取第一行，避免把上千字符填进输入框。
    const QString text = clipboard->text().section(QLatin1Char('\n'), 0, 0).trimmed();
    if (text.isEmpty()) {
        setStatus(QStringLiteral("剪贴板里没有文本内容。"), true);
        return;
    }
    const QColor color = colortext::parseColor(text);
    if (!color.isValid()) {
        setStatus(QStringLiteral("剪贴板里不是可识别的颜色：%1").arg(text.left(60)), true);
        return;
    }
    stopPicking();
    m_hexEdit->setText(text);
    applyColor(color, true, false);   // 保留用户粘贴的原始写法，只同步右侧格式
    m_loupe->setSolid(m_color);
    setStatus(QStringLiteral("已从剪贴板读入 %1。").arg(colortext::formatHex(m_color)));
}

void ColorPickerPanel::copyText(const QString &text, const QString &label)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        setStatus(QStringLiteral("剪贴板不可用。"), true);
        return;
    }
    clipboard->setText(text);
    // QClipboard::setText() 没有返回值：Windows 上剪贴板被别的进程占住时，
    // Qt 只往 stderr 打一条 OleSetClipboard 警告，界面若照常报「已复制」就是
    // 静默失败（用户粘贴出来还是旧内容）。回读一次确认真的写进去了。
    if (clipboard->text() != text) {
        setStatus(QStringLiteral("剪贴板写入失败（可能被其他程序占用），请重试。"), true);
        return;
    }
    setStatus(QStringLiteral("已复制 %1：%2").arg(label, text));
}

void ColorPickerPanel::copyAllFields()
{
    QStringList lines;
    const QList<colortext::Field> fields = colortext::formatFields(m_color);
    for (const colortext::Field &field : fields)
        lines.append(QStringLiteral("%1: %2").arg(field.label, field.text));
    copyText(lines.join(QLatin1Char('\n')), QStringLiteral("全部格式"));
}

void ColorPickerPanel::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setProperty("state", error ? QStringLiteral("error") : QStringLiteral("ok"));
    repolish(m_status);
}

// ---------------------------------------------------------------- 事件

void ColorPickerPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 打开工具即自动进入「吸管模式」：用户点开取色器就是要取色，没必要先看到静态
    // 面板再手动点按钮。延一轮事件循环（singleShot(0)）等宿主把面板 addWidget 进页栈、
    // 布局/绘制完成后再 showEyedropper + startPicking，避免读到未绘制的窗口像素，
    // 也让初始色块先画出来。
    // m_autoPicked 保证只自动触发一次：锁定后吸管浮窗已关闭，宿主窗口也不会再 show；
    // 保留挡门防宿主在结束前又触发一次 showEvent 被误判成「再次打开取色器」。
    if (!m_autoPicked) {
        m_autoPicked = true;
        QTimer::singleShot(0, this, [this] {
            if (isVisible() && !m_picking) {
                showEyedropper();
                startPicking();
            }
        });
    }
}

void ColorPickerPanel::hideEvent(QHideEvent *event)
{
    // 面板被页栈藏起来后继续抓屏既浪费又可能干扰用户，立即停止采样。
    // 唯一例外是「吸管模式」：窗口是 showEyedropper() 自己 hide 的，此刻正在取色，
    // 若在这里 stopPicking 就等于一进吸管模式就自己把自己停了，永远取不到色。
    if (!m_eyedropperMode && m_picking) {
        stopPicking();
        if (m_status->property("state").toString() != QLatin1String("error"))
            setStatus(QStringLiteral("取色已暂停；点「屏幕取色」可重新开始。"));
    }
    QWidget::hideEvent(event);
}

void ColorPickerPanel::keyPressEvent(QKeyEvent *event)
{
    // 只认裸 Esc（与宿主的分层 Esc 约定一致）：取色态下先取消取色并吞掉事件，
    // 不让它冒泡到 MainWindow 变成「退回首页」。
    if (m_picking && event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        stopPicking();
        hideEyedropper();
        setStatus(QStringLiteral("已取消取色。"));
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

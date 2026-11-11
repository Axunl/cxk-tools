#include "ScreenshotPanel.h"

#include "CaptureOverlay.h"
#include "ScrollSession.h"
#include "ThemePalette.h"
#include "LayoutMetrics.h"   // 颜色令牌（宿主与插件共用）

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {

/// 动态属性（state / accent）改完后必须重新 polish，否则 qss 不会重算样式。
/// 与取色器 / 内存监控插件里同名的辅助函数一个道理。
void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

/// 空闲时的引导文案。截图工具最大的门槛是「不知道能干什么」，
/// 这里把两条链路和「选区内必须是一块屏」的限制一次讲清楚。
QString idleHint()
{
    return QStringLiteral(
        "点「区域截图」框选屏幕上任意一块区域；或点「滚动截图」框选一整段可滚动的内容，"
        "然后自己用鼠标滚轮往下滚，程序会把每次新出现的部分自动接成一张长图。\n\n"
        "截图完成后会自动复制到剪贴板，也可以在这里另存为 PNG。\n"
        "选区限制在一屏之内（跨屏时选区停在起始屏幕边缘）。");
}

} // namespace

ScreenshotPanel::ScreenshotPanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    updatePreview();
    updateActions();
    setStatus(QStringLiteral("就绪 · 截图会先隐藏本窗口，按 Esc 可取消框选"));
}

void ScreenshotPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐（与其它插件的约定一致）
    layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    layout->setSpacing(10);

    // ---------------------------------------------------------------- 工具条
    auto *bar = new QHBoxLayout;
    bar->setSpacing(8);

    m_regionBtn = new QPushButton(QStringLiteral("区域截图"), this);
    m_regionBtn->setProperty("accent", true);   // 全局 qss 的强调按钮样式
    m_regionBtn->setToolTip(QStringLiteral("隐藏窗口后拖拽框选屏幕区域，完成后自动复制到剪贴板"));
    bar->addWidget(m_regionBtn);

    m_scrollBtn = new QPushButton(QStringLiteral("滚动截图"), this);
    m_scrollBtn->setToolTip(QStringLiteral("框选区域后，在目标窗口里手动滚动鼠标滚轮，"
                                           "程序把新出现的内容自动拼成长图"));
    bar->addWidget(m_scrollBtn);

    bar->addStretch(1);

    m_copyBtn = new QPushButton(QStringLiteral("复制"), this);
    m_saveBtn = new QPushButton(QStringLiteral("保存 PNG"), this);
    m_clearBtn = new QPushButton(QStringLiteral("清空"), this);
    bar->addWidget(m_copyBtn);
    bar->addWidget(m_saveBtn);
    bar->addWidget(m_clearBtn);
    layout->addLayout(bar);

    // ---------------------------------------------------------------- 预览区
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    // 图比视口小时居中显示，比视口大时出滚动条（长截图靠它预览）
    m_scrollArea->setAlignment(Qt::AlignCenter);
    xtools::theme::applyStyle(m_scrollArea, QStringLiteral(
        "QScrollArea{background:@bg-alt;border:1px solid @border;border-radius:10px;}"));
    // 视口本身默认继承调色板底色，在深色主题下会和上面的边框形成一块浅色；
    // 置透明让它透出 QScrollArea 的底色。
    m_scrollArea->viewport()->setStyleSheet(QStringLiteral("background:transparent;"));

    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setWordWrap(true);
    xtools::theme::applyStyle(m_preview, QStringLiteral(
        "QLabel{color:@text-faint;font-size:12px;background:transparent;padding:16px;}"));
    m_scrollArea->setWidget(m_preview);
    layout->addWidget(m_scrollArea, 1);

    // ---------------------------------------------------------------- 状态行
    m_status = new QLabel(this);
    xtools::theme::applyStyle(m_status, QStringLiteral(
        "QLabel{color:@text-muted;font-size:12px;background:transparent;}"
        "QLabel[state=\"error\"]{color:@danger-text;}"));
    layout->addWidget(m_status);

    connect(m_regionBtn, &QPushButton::clicked, this, [this] { startCapture(false); });
    connect(m_scrollBtn, &QPushButton::clicked, this, [this] { startCapture(true); });
    connect(m_copyBtn, &QPushButton::clicked, this, &ScreenshotPanel::copyImage);
    connect(m_saveBtn, &QPushButton::clicked, this, &ScreenshotPanel::saveImage);
    connect(m_clearBtn, &QPushButton::clicked, this, &ScreenshotPanel::clearImage);
}

ScreenshotPanel::~ScreenshotPanel()
{
    // 浮层 / 会话都不设 parent（见 CaptureOverlay.h），这里必须自己收掉。
    // 注意先判断状态再置空：置空后 m_session 就查不出「是否还在跑」了。
    const bool sessionActive = !m_session.isNull();

    if (m_overlay) {
        m_overlay->deleteLater();
        m_overlay = nullptr;
    }
    if (m_session) {
        m_session->deleteLater();
        m_session = nullptr;
    }
    if (m_capturePending || sessionActive)
        restoreHostWindow();
}

void ScreenshotPanel::startCapture(bool scrolling)
{
    // 已有遮罩或会话在跑时忽略重复点击：第二次点击会让第一个遮罩变成孤儿，
    // 用户永远关不掉它。
    if (m_capturePending || m_overlay || m_session)
        return;

    m_pendingScroll = scrolling;
    m_hostWindow = window();
    m_hostWasVisible = m_hostWindow && m_hostWindow->isVisible();
    m_capturePending = true;
    if (m_hostWindow)
        m_hostWindow->hide();   // 不 hide 的话遮罩会盖住本窗口，用户截不到背后的内容

    setStatus(scrolling ? QStringLiteral("准备滚动截图，请稍候…")
                        : QStringLiteral("准备截图，请稍候…"));

    // 延迟抓屏：hide() 之后桌面需要重绘一轮，立刻 grabWindow 会截到主窗口残影。
    // 以 this 为 context，面板若在等待期间被销毁，这个回调自动作废。
    QTimer::singleShot(kCaptureDelayMs, this, [this] { beginOverlay(); });
}

void ScreenshotPanel::beginOverlay()
{
    if (!m_capturePending)
        return;

    m_snapshot = screengrab::captureAll();
    if (!m_snapshot.isValid()) {
        restoreHostWindow();
        // 抓不到屏时必须给可读原因，不能静默失败（规则 3）
        setStatus(QStringLiteral("屏幕抓取失败：") + screengrab::platformHint(), true);
        return;
    }

    const QString tip = m_pendingScroll
        ? QStringLiteral("拖动框选要拼接的区域（把整块可滚动内容都框进来）· 松开后自己滚动鼠标滚轮 · Esc 取消")
        : QStringLiteral("拖动鼠标框选要截取的区域 · Esc 取消");

    auto *overlay = new CaptureOverlay(
        m_snapshot, tip,
        [this](const QRect &region) { onRegionSelected(region, m_pendingScroll); },
        [this] {
            restoreHostWindow();
            setStatus(QStringLiteral("已取消截图"));
        });
    m_overlay = overlay;
    overlay->show();
    overlay->raise();
    overlay->activateWindow();
    overlay->setFocus();   // Esc 取消依赖遮罩拿到键盘焦点
}

void ScreenshotPanel::onRegionSelected(const QRect &region, bool scrolling)
{
    m_capturePending = false;

    // 裁剪用的是「框选时冻结的那一张」，与用户在遮罩里看到的一致
    qreal dpr = 1.0;
    const QImage cropped = m_snapshot.crop(region, &dpr);
    m_snapshot = screengrab::Snapshot();   // 快照可能几十 MB，裁完立刻释放

    if (cropped.isNull()) {
        restoreHostWindow();
        setStatus(QStringLiteral("裁剪失败：选区超出屏幕范围，请重试"), true);
        return;
    }

    if (!scrolling) {
        restoreHostWindow();
        setImage(cropped, QString(), true);
        return;
    }

    // 滚动截图：宿主窗口保持隐藏，等会话结束再恢复（否则会截到自己的窗口）
    m_session = new ScrollSession(region, dpr, kMaxScrollHeight);
    connect(m_session, &ScrollSession::finished, this,
            [this](const QImage &image, bool limitReached) {
                restoreHostWindow();
                if (image.isNull()) {
                    setStatus(QStringLiteral("拼接结果为空，请重新框选后重试"), true);
                    return;
                }
                setImage(image, limitReached
                                   ? QStringLiteral("已达高度上限，长图可能不完整")
                                   : QString(), true);
            });
    connect(m_session, &ScrollSession::cancelled, this, [this] {
        restoreHostWindow();
        setStatus(QStringLiteral("已取消滚动截图"));
    });
    connect(m_session, &ScrollSession::failed, this, [this](const QString &reason) {
        restoreHostWindow();
        setStatus(reason, true);
    });
    m_session->show();

    setStatus(QStringLiteral("滚动拼接中：在目标窗口里用鼠标滚轮滚动内容（上下都行），"
                             "停止约 2.5 秒自动完成，也可以点控制条上的「完成」"));
}

void ScreenshotPanel::restoreHostWindow()
{
    m_capturePending = false;
    if (m_hostWindow && m_hostWasVisible) {
        m_hostWindow->show();
        m_hostWindow->raise();
        m_hostWindow->activateWindow();
    }
    m_hostWindow = nullptr;
}

void ScreenshotPanel::setImage(const QImage &image, const QString &note, bool autoCopy)
{
    m_image = image;
    updatePreview();
    updateActions();

    if (autoCopy && !m_image.isNull())
        QApplication::clipboard()->setImage(m_image);

    // 尺寸按**设备像素**报：用户关心的是「这张图有多少像素」，不是逻辑尺寸
    QString text = QStringLiteral("%1 · %2 × %3 像素")
                       .arg(autoCopy ? QStringLiteral("已复制到剪贴板")
                                     : QStringLiteral("已生成"))
                       .arg(m_image.width())
                       .arg(m_image.height());
    if (!note.isEmpty())
        text += QStringLiteral(" · ") + note;
    setStatus(text);
}

void ScreenshotPanel::updatePreview()
{
    if (m_image.isNull()) {
        m_preview->setPixmap(QPixmap());
        m_preview->setText(idleHint());
        m_preview->setMinimumSize(0, 0);
        return;
    }

    m_preview->setText(QString());
    m_preview->setPixmap(QPixmap::fromImage(m_image));
    // 显式给出最小尺寸：QScrollArea 的 widgetResizable 只保证「不比视口小」，
    // 长图要靠控件最小尺寸把滚动条撑出来，否则会被裁在视口里看不出全貌。
    m_preview->setMinimumSize(m_image.deviceIndependentSize().toSize());
}

void ScreenshotPanel::updateActions()
{
    const bool has = !m_image.isNull();
    m_copyBtn->setEnabled(has);
    m_saveBtn->setEnabled(has);
    m_clearBtn->setEnabled(has);
}

void ScreenshotPanel::copyImage()
{
    if (m_image.isNull())
        return;
    QApplication::clipboard()->setImage(m_image);
    setStatus(QStringLiteral("已复制到剪贴板 · %1 × %2 像素")
                  .arg(m_image.width())
                  .arg(m_image.height()));
}

void ScreenshotPanel::saveImage()
{
    if (m_image.isNull())
        return;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    const QString suggested = QDir(dir).filePath(
        QStringLiteral("cxk-shot-%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));

    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存截图"), suggested,
                                                QStringLiteral("PNG 图片 (*.png)"));
    if (path.isEmpty())
        return;
    // 用户可能把扩展名删了，补回来（QImage::save 靠后缀判格式，这里显式给 "PNG"）
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");

    if (m_image.save(path, "PNG"))
        setStatus(QStringLiteral("已保存到 %1").arg(QDir::toNativeSeparators(path)));
    else
        setStatus(QStringLiteral("保存失败：无法写入 %1").arg(QDir::toNativeSeparators(path)), true);
}

void ScreenshotPanel::clearImage()
{
    m_image = QImage();
    updatePreview();
    updateActions();
    setStatus(QStringLiteral("已清空，可以重新截图"));
}

void ScreenshotPanel::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setProperty("state", error ? QStringLiteral("error") : QStringLiteral("ok"));
    repolish(m_status);
}

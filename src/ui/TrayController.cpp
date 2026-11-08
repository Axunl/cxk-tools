#include "TrayController.h"

#include "IconBadge.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>

TrayController::TrayController(QObject *parent)
    : QObject(parent)
{
    // 托盘图标复用主窗口那套字母徽章，保证任务栏/托盘/标题栏图标一致
    m_tray = new QSystemTrayIcon(IconBadge::icon(QStringLiteral("cxk-tools"), QStringLiteral("C")), this);
    m_tray->setToolTip(QStringLiteral("CXK 工具箱"));

    // 注意：QMenu 不设 parent。它的生命周期跟着托盘图标，
    // 设成 this 的子对象在某些平台上菜单会随对象析构提前消失。
    m_menu = new QMenu();

    m_launcherAction = m_menu->addAction(QStringLiteral("呼出搜索"));
    connect(m_launcherAction, &QAction::triggered, this, &TrayController::launcherRequested);

    auto *mainAction = m_menu->addAction(QStringLiteral("打开主窗口"));
    connect(mainAction, &QAction::triggered, this, &TrayController::mainWindowRequested);

    m_menu->addSeparator();

    auto *settingsAction = m_menu->addAction(QStringLiteral("设置"));
    connect(settingsAction, &QAction::triggered, this, &TrayController::settingsRequested);

    // 更新检查放在设置下面：低频操作，但不能藏进设置页（用户找不到就等于没有）
    auto *updateAction = m_menu->addAction(QStringLiteral("检查更新"));
    connect(updateAction, &QAction::triggered, this, &TrayController::checkUpdateRequested);

    // 不做热重载，"重新扫描"实际是重启进程
    auto *restartAction = m_menu->addAction(QStringLiteral("重新扫描插件（重启）"));
    connect(restartAction, &QAction::triggered, this, &TrayController::restartRequested);

    m_menu->addSeparator();

    auto *quitAction = m_menu->addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, this, &TrayController::quitRequested);

    m_tray->setContextMenu(m_menu);

    // 左键单击/双击托盘图标 = 打开主窗口（Windows 习惯；macOS 上点击通常弹菜单）
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        switch (reason) {
        case QSystemTrayIcon::Trigger:
        case QSystemTrayIcon::DoubleClick:
            emit mainWindowRequested();
            break;
        default:
            break;
        }
    });

    // 点气泡 = 对这条提示做点什么。目前只有「发现新版本」会用到：
    // 由 AppController 接住后直接把更新对话框弹出来，用户点一下就能装。
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, &TrayController::messageClicked);

    m_tray->show();
}

void TrayController::setHotkeyText(const QString &text)
{
    if (m_launcherAction)
        m_launcherAction->setText(QStringLiteral("呼出搜索（%1）").arg(text));
}

void TrayController::showMessage(const QString &title, const QString &body, bool warning)
{
    if (!m_tray)
        return;

    // 4 秒：太短读不完，太长挡住右下角
    m_tray->showMessage(title,
                        body,
                        warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information,
                        4000);
}

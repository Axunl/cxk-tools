// ---------------------------------------------------------------------------
// 程序入口。
//
// 启动顺序有讲究：
//   1. QApplication 之后立刻设置 org/app 名 —— QStandardPaths 依赖它来算配置/日志
//      目录，晚于此的所有落盘都会跑到别的目录里；
//   2. 装日志处理器，保证后续部件的告警都能落盘；
//   3. 载入 qss；
//   4. 抢单实例锁（抢不到就通知已有实例并退出）；
//   5. 交给 AppController 组装。
// ---------------------------------------------------------------------------

#include "AppController.h"
#include "IconBadge.h"
#include "Log.h"
#include "SingleInstance.h"

#include <QApplication>
#include <QDebug>
#include <QFile>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setOrganizationName(QStringLiteral("cxk"));
    QApplication::setOrganizationDomain(QStringLiteral("cxk.local"));
    QApplication::setApplicationName(QStringLiteral("cxk-tools"));
    QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setApplicationDisplayName(QStringLiteral("CXK 工具箱"));
    QApplication::setWindowIcon(IconBadge::icon(QStringLiteral("cxk-tools"), QStringLiteral("C")));

    // 主窗口"关闭"只是收进托盘，不能让最后一个窗口关闭就退出进程
    QApplication::setQuitOnLastWindowClosed(false);

    Log::init();

    QFile style(QStringLiteral(":/style.qss"));
    if (style.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // 显式 fromUtf8：源码是 UTF-8，不能让 Qt 猜
        qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
    } else {
        // 样式缺失不致命（界面会变成系统默认样子），但必须留痕
        qWarning() << "main: 无法加载样式表 :/style.qss";
    }

    SingleInstance guard(QStringLiteral("cxk-tools-single-instance"));
    if (!guard.tryLock()) {
        qInfo() << "main: 已有实例在运行，通知其唤出主窗口后退出";
        return 0;
    }

    // --autostart 由开机自启项传入：只驻留托盘，不弹窗打扰用户
    const bool autostart = app.arguments().contains(QStringLiteral("--autostart"));

    AppController controller;
    guard.setActivateHandler([&controller] { controller.showMainWindow(); });
    controller.start(autostart);

    return app.exec();
}

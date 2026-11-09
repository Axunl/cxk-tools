// ---------------------------------------------------------------------------
// 程序入口。
//
// 启动顺序有讲究：
//   1. QApplication 之后立刻设置 org/app 名 —— QStandardPaths 依赖它来算配置/日志
//      目录，晚于此的所有落盘都会跑到别的目录里；
//   2. 装日志处理器，保证后续部件的告警都能落盘；
//   3. 载入主题（调色板 + qss）；
//   4. 抢单实例锁（抢不到就通知已有实例并退出）；
//   5. 交给 AppController 组装。
// ---------------------------------------------------------------------------

#include "AppController.h"
#include "IconBadge.h"
#include "Log.h"
#include "SingleInstance.h"
#include "Theme.h"

#include <QApplication>
#include <QDebug>

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

    // 主题必须在任何窗口创建之前落地：卡片、磁贴这类控件只在构造时读一次
    // 调色板/样式，晚一步就会有一批控件停在旧配色上。
    const QStringList args = app.arguments();
    Theme &theme = Theme::instance();
    theme.load();

    // --theme <system|light|dark>：只覆盖本次运行，不写回配置。
    // 界面回归（--shot）需要一个固定的配色，不能依赖构建机的系统设置。
    bool themeOverridden = false;
    const int themeIndex = args.indexOf(QStringLiteral("--theme"));
    if (themeIndex >= 0 && themeIndex + 1 < args.size()) {
        bool ok = false;
        const Theme::Mode mode = Theme::modeFromString(args.at(themeIndex + 1), &ok);
        if (ok) {
            theme.setMode(mode, false);   // 内含一次 apply()
            themeOverridden = true;
        } else {
            qWarning() << "main: 无法识别的 --theme 取值:" << args.at(themeIndex + 1)
                       << "（可用 system / light / dark）";
        }
    }

    if (!themeOverridden)
        theme.apply();

    // --check-update / --shot 是「跑一次就退出」的诊断命令：它们不该被正在运行的实例挡住，
    // 否则想用脚本验证更新链路或做界面回归时，只要用户开着程序就永远拿到"已有实例在运行"。
    const bool diagnostic = args.contains(QStringLiteral("--check-update"))
                            || args.contains(QStringLiteral("--shot"));

    SingleInstance guard(QStringLiteral("cxk-tools-single-instance"));
    if (!diagnostic && !guard.tryLock()) {
        qInfo() << "main: 已有实例在运行，通知其唤出主窗口后退出";
        return 0;
    }

    // --autostart 由开机自启项传入：只驻留托盘，不弹窗打扰用户
    const bool autostart = args.contains(QStringLiteral("--autostart"));

    AppController controller;
    if (!diagnostic)
        guard.setActivateHandler([&controller] { controller.showMainWindow(); });
    controller.start(autostart);

    return app.exec();
}

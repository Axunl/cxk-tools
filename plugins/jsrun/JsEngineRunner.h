#pragma once

// ---------------------------------------------------------------------------
// JS 运行插件的执行内核：引擎、工作线程、跨线程中断、console 桥。
//
// 为什么要把脚本放到后台线程执行：QJSEngine 没有「执行时限」参数，死循环会
// 一直占着调用它的线程。若在 UI 线程里 evaluate()，用户写一个 while(true) 就
// 只能强杀进程。Qt 官方为这种情况提供了 QJSEngine::setInterrupted()——
// 文档原文明确「This function is thread safe. You may call it from a different
// thread in order to interrupt, for example, an infinite loop in JavaScript.」
// 所以：脚本在 worker 线程跑，主线程超时后跨线程打断它。
//
// 线程模型（改动前请先读完）：
//   * QJSEngine 只能在**创建它的线程**里使用，所以引擎在 JsRunWorker::run()
//     里以栈对象创建，run() 返回时自动销毁；绝不跨线程传递引擎对象本身。
//   * 主线程唯一能碰的是 RunContext 里那个裸指针，而且必须持 mutex：
//     取指针 + 调 setInterrupted 与 worker 的「建/摘指针」互斥，否则主线程可能
//     在引擎析构后仍然拿着旧指针。evaluate() 期间绝不持锁，否则自己挡住中断。
//   * 每次运行新建一个引擎：setInterrupted(true) 会一直生效到显式设回 false，
//     且被中断的引擎里用户 var / 全局状态都会污染下一次运行。
//   * 绝不用 QThread::terminate()：那会撕掉 V4 的堆，宿主必崩。等不到线程退出
//     就与它脱钩（ctx 由 shared_ptr 保活），让它在后台自行结束。
// ---------------------------------------------------------------------------

#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>

QT_BEGIN_NAMESPACE
class QJSEngine;
QT_END_NAMESPACE

namespace jsrun {

/// 执行超时。3 秒足够覆盖常见的几十万次循环/字符串处理，又不至于让用户以为卡死。
inline constexpr int kTimeoutMs = 3000;

/// 面板销毁时等待 worker 退出的上限。超时后脱钩而不是继续等，避免关窗口卡住。
inline constexpr int kShutdownWaitMs = 2000;

enum class RunStatus
{
    Ok,       ///< 正常结束（含 undefined 返回值）
    Error,    ///< 语法或运行时错误
    Timeout   ///< 因超时被中断
};

/// run() 的产物。跨线程按值传递，只包含已经格式化好的文本，界面拿到就能显示。
struct RunResult
{
    RunStatus status = RunStatus::Ok;
    QString valueText;   ///< 返回值文本（status == Ok 时有意义）
    QString errorText;   ///< 中文错误说明，含行号与少量栈帧（status != Ok 时有意义）
    qint64 elapsedMs = 0; ///< evaluate() 本身的耗时，不含线程启动
};

/// worker 与主线程唯一共享的对象。
///
/// engine 指针的生命周期契约：worker 创建后写入、结束前清零；主线程只在持锁时
/// 读它并立刻调用 setInterrupted。谁都不许在锁外保存这个指针。
struct RunContext
{
    QMutex mutex;
    QJSEngine *engine = nullptr;
    /// 主线程在超时时置位，worker 据此把结果判成 Timeout 而不是普通错误。
    std::atomic_bool timedOut{false};
    /// 「已经请求过中断」的粘滞标记。存在的理由：interrupt() 可能在 worker 还没
    /// 建好引擎时就被调用（例如刚点运行就切走页面），那次 setInterrupted 无处可
    /// 施加；worker 建好引擎后必须补一次，否则死循环会一直跑到进程退出。
    std::atomic_bool interrupted{false};

    /// 跨线程打断当前执行。没有在跑（engine 已被摘掉）时只记标记。
    void interrupt();
};

/// console 桥：脚本侧只能通过它送两段字符串过来，拿不到任何宿主能力
///（桥对象本身没有暴露成全局变量，只作为安装函数的参数出现）。
class JsConsoleBridge : public QObject
{
    Q_OBJECT

public:
    explicit JsConsoleBridge(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

public slots:
    /// level 只接受 log / warn / error（白名单在实现里，防止以后改片段时把任意
    /// 字符串当颜色用），text 由 JS 侧格式化好。
    void log(const QString &level, const QString &text);

signals:
    void line(const QString &level, const QString &text);
};

/// 一次执行的载体。继承 QThread 而不是 moveToThread + worker 对象：引擎天然
/// 被限制在 run() 的线程里，也不需要额外管理 worker 对象的删除时机。
class JsRunWorker : public QThread
{
    Q_OBJECT

public:
    JsRunWorker(QString code, std::shared_ptr<RunContext> context);

    /// 构造后由面板调用 start()。QThread::run() 在**新线程**执行，
    /// 内部没有事件循环；所有信号都靠队列送回主线程。
    void run() override;

signals:
    void logLine(const QString &level, const QString &text);
    void finished(const jsrun::RunResult &result);

private:
    QString m_code;
    std::shared_ptr<RunContext> m_context;
};

} // namespace jsrun

Q_DECLARE_METATYPE(jsrun::RunResult)

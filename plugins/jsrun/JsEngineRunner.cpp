#include "JsEngineRunner.h"

#include "JsRunConsole.h" // console 预置片段 + 截断阈值

#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>
#include <QJSValueList>
#include <QMutexLocker>

#include <utility>

namespace {

/// 把 evaluate() 返回的错误对象整理成可直接显示的中文说明。
///
/// Qt 的错误对象带 name / message / lineNumber / stack（V4 在 Qt 6 里都会填）。
/// 行号是用户最需要的信息——脚本里贴一大段代码时，「TypeError」本身说明不了
/// 问题出在哪一行。栈只挑 "at ..." 帧，最多 6 行：完整栈会把输出区刷满，
/// 而这个工具里用户写的都是几十行的小脚本，6 帧足够定位。
QString formatError(const QJSValue &error)
{
    const QString name = error.property(QStringLiteral("name")).toString();
    const QString message = error.property(QStringLiteral("message")).toString();
    const int line = error.property(QStringLiteral("lineNumber")).toInt();

    QString head = message.isEmpty() ? error.toString() : message;
    if (!name.isEmpty() && !head.startsWith(name))
        head = name + QStringLiteral("：") + head;

    QString text = line > 0 ? QStringLiteral("第 %1 行：%2").arg(line).arg(head)
                            : QStringLiteral("执行出错：%1").arg(head);

    const QStringList frames = error.property(QStringLiteral("stack"))
                                   .toString()
                                   .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    int shown = 0;
    for (const QString &frame : frames) {
        const QString trimmed = frame.trimmed();
        if (!trimmed.startsWith(QLatin1String("at ")))
            continue; // 第一行是重复的 message，跳过
        if (shown >= 6) {
            text += QStringLiteral("\n  …");
            break;
        }
        text += QStringLiteral("\n  ") + trimmed;
        ++shown;
    }
    return text;
}

/// 格式化脚本的最终返回值。
///
/// 字符串原样输出（用户写 `"abc"` 想看的就是 abc，不是带引号的 "abc"）；
/// 其它类型交给 JS 侧的 describe()，与 console.log 用同一套规则。
/// describe 在安装失败时可能是空值，这时退回 QJSValue::toString()。
QString formatResult(const QJSValue &describe, const QJSValue &value)
{
    if (value.isUndefined())
        return QStringLiteral("（无返回值）");
    if (value.isNull())
        return QStringLiteral("null");
    if (value.isString())
        return jsrun::truncateForUi(value.toString(), jsrun::kMaxResultChars);

    QString text;
    if (describe.isCallable()) {
        const QJSValue formatted = describe.call(QJSValueList{value});
        text = (formatted.isError() || formatted.isUndefined()) ? value.toString()
                                                               : formatted.toString();
    } else {
        text = value.toString();
    }
    return jsrun::truncateForUi(text, jsrun::kMaxResultChars);
}

} // namespace

// ---------------------------------------------------------------- RunContext

void jsrun::RunContext::interrupt()
{
    // 先记粘滞标记，再尝试打到引擎上：两者之间 worker 可能刚好还没建好引擎
    //（或已经摘掉指针），那时这次调用会落空，但标记会留在原处，worker 建好
    // 引擎后会补打一次——否则「刚点运行就切走页面」会让死循环永远停不下来。
    interrupted.store(true);

    // 持锁的唯一目的：与 worker 的「建引擎 / 摘引擎」互斥。若在锁外读指针，
    // worker 可能刚好在指针被复制之后、setInterrupted 之前把引擎析构掉。
    // evaluate() 期间 worker 不持锁，所以这里不会把中断自己挡在门外。
    QMutexLocker locker(&mutex);
    if (engine)
        engine->setInterrupted(true);
}

// ------------------------------------------------------------ JsConsoleBridge

void jsrun::JsConsoleBridge::log(const QString &level, const QString &text)
{
    // 白名单：level 会被界面拿去映射颜色，不能让任意字符串透过去
    const QString safe = (level == QLatin1String("warn") || level == QLatin1String("error"))
                             ? level
                             : QStringLiteral("log");
    emit line(safe, truncateForUi(text, kMaxLogLineChars));
}

// -------------------------------------------------------------- JsRunWorker

jsrun::JsRunWorker::JsRunWorker(QString code, std::shared_ptr<RunContext> context)
    : m_code(std::move(code))
    , m_context(std::move(context))
{
    // 结果要经队列连接拷回主线程，自定义类型必须先注册（重复注册无副作用）。
    // 放在构造函数里而不是某个全局初始化点：只有真正跑脚本时才有这个需求。
    qRegisterMetaType<jsrun::RunResult>("jsrun::RunResult");
}

void jsrun::JsRunWorker::run()
{
    QElapsedTimer timer;
    timer.start();

    // 引擎是栈对象：QJSEngine 只能在创建它的线程使用，这样写天然保证
    // 「本线程创建、本线程销毁」，不需要任何跨线程的所有权约定。
    QJSEngine engine;
    {
        QMutexLocker locker(&m_context->mutex);
        m_context->engine = &engine;
        // 补打「运行还没起来就收到中断」的那次请求（见 RunContext::interrupted）
        if (m_context->interrupted.load())
            engine.setInterrupted(true);
    }

    // console 桥也是栈对象。newQObject() 默认把所有权交给 JS
    //（JavaScriptOwnership），引擎 GC/析构时会对它 delete —— 那是个栈对象，
    // 双重释放必崩。这一行是本文件最容易漏、也最致命的一行。
    JsConsoleBridge bridge;
    QJSEngine::setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    // DirectConnection：run() 里没有事件循环，排队会把日志压到脚本结束后才派发，
    // 流式输出就没了。信号本身会在主线程侧再排一次队（面板用 QueuedConnection）。
    connect(&bridge, &JsConsoleBridge::line, this, &JsRunWorker::logLine,
            Qt::DirectConnection);

    QJSValue describe;
    const QJSValue installer =
        engine.evaluate(QString::fromUtf8(kConsoleInstaller), QStringLiteral("<jsrun-console>"));
    if (installer.isError()) {
        qWarning() << "jsrun: 安装 console 失败:" << installer.toString();
    } else {
        QJSValueList args;
        args << engine.newQObject(&bridge);
        const QJSValue api = installer.call(args);
        if (api.isError()) {
            qWarning() << "jsrun: 初始化 console 失败:" << api.toString();
        } else {
            // 挂全局放在 C++ 侧：V4 里没有 globalThis（实测 typeof 是 undefined），
            // 让 JS 片段自己往全局写只能靠非严格模式的 this，属于实现细节。
            QJSValue global = engine.globalObject();
            const QJSValue logger = api.property(QStringLiteral("logger"));
            global.setProperty(QStringLiteral("console"), logger);
            // print 是 Qt 脚本环境的习惯写法，给个等价别名，免得从 Qt 文档抄来的
            // 示例第一行就报 undefined is not a function
            global.setProperty(QStringLiteral("print"), logger.property(QStringLiteral("log")));

            describe = api.property(QStringLiteral("describe"));
            if (describe.isError()) {
                qWarning() << "jsrun: 格式化函数不可用:" << describe.toString();
                describe = QJSValue();
            }
        }
    }

    const QJSValue value = engine.evaluate(m_code, QStringLiteral("script.js"));
    const qint64 elapsed = timer.elapsed();

    RunResult result;
    result.elapsedMs = elapsed;
    if (m_context->timedOut.load()) {
        // 中断也会让 evaluate 返回 error，但那是「被我们打断」而不是脚本的错，
        // 必须分开提示，否则用户会以为自己的语法有问题。
        result.status = RunStatus::Timeout;
        result.errorText = QStringLiteral("执行超过 %1 秒，已中断。常见原因是死循环或数据量过大。")
                               .arg(kTimeoutMs / 1000);
    } else if (value.isError()) {
        result.status = RunStatus::Error;
        result.errorText = formatError(value);
    } else {
        result.status = RunStatus::Ok;
        result.valueText = formatResult(describe, value);
    }

    // 先摘指针、再让引擎析构：此后主线程再调 interrupt() 只会看到 nullptr。
    {
        QMutexLocker locker(&m_context->mutex);
        m_context->engine = nullptr;
    }

    emit finished(result);
}

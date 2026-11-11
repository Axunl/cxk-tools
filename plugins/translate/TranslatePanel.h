#pragma once

// ---------------------------------------------------------------------------
// 翻译工具面板（插件内部，宿主只认 ITool）。
//
// 交互照参考的有道翻译文本页：顶部源/目标语言与交换按钮，中间左右双栏
// （左输入、右译文），底部字数与状态。刻意不做「通用场景」「文档翻译」等
// AI 功能：本插件只做纯文本翻译，宿主里也没有对应的账号/文件能力。
//
// 网络侧的几条硬约束（实测，详见 TranslateLogic.h 文件头）：
//   * 有道演示接口有频率限制（411）与文本长度上限（103），搜狗要先握手拿会话且
//     0.1 秒连发会偶发 status=-1，MyMemory 单次 500 字符；所以长文本按 400 字符
//     分段串行请求，段间留间隔，且失败自动降级到下一个服务；
//   * 搜狗的 secretCode / uuid 只在面板实例里缓存（宿主每次进入都新建面板），
//     禁止静态/全局变量，也避免内嵌与独立窗口两份面板串会话；
//   * 腾讯翻译君已关闭免费 API 且网页需滑块验证码，不接数据源；应要求已移除
//     「腾讯网页」跳转按钮，面板只保留四家真正可用的服务；
//   * 异步回复一律带 generation：面板销毁或用户重新发起翻译后，旧回复不许改写界面。
//
// 面板类放头文件是为了按 AGENTS.md 规则 2 用临时工程做端到端断言（开 AUTOMOC 即可），
// 而不是只测纯逻辑。
// ---------------------------------------------------------------------------

#include "TranslateLogic.h"

#include <QNetworkAccessManager>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTimer;

class TranslatePanel : public QWidget
{
    Q_OBJECT

public:
    explicit TranslatePanel(QWidget *parent = nullptr);
    ~TranslatePanel() override;

    /// 宿主把首页搜索框里的英文带进来时调用：先把目标语言切到中文再填进输入框并
    /// 立刻翻译（推荐进来的语义是「把这段英文翻成中文」）。空文本直接忽略。
    /// 注意：用户主动点磁贴打开不会调用本函数，那时面板保持默认目标语言「英语」，
    /// 因此「推荐进来 → 译中文、主动进来 → 译英文」两种默认值靠是否有本次调用来区分。
    void setInputText(const QString &text);

protected:
    /// 面板一显示就把焦点放进输入框。同步 setFocus 会被宿主的焦点链覆盖，
    /// 必须延后一拍（与 JsonFmtPanel::showEvent 同一处理）。
    void showEvent(QShowEvent *event) override;
    /// 三个下拉框的箭头是自绘子控件，得跟着下拉框的尺寸与可用状态走
    /// （全局 qss 写了 ::drop-down 规则后，Qt 不再画默认三角）。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // ------------------------------------------------------------ 界面
    void buildUi();
    int  sourceIndex() const;   ///< -1 = 自动检测
    int  targetIndex() const;
    int  engineChoice() const;  ///< -1 = 自动降级，否则是 translate::Engine 的值
    void updateCount();
    void setStatus(const QString &text, const QString &state = QStringLiteral("idle"));
    void setBusy(bool busy);

    // ------------------------------------------------------------ 翻译调度
    /// 收集输入、分段、决定引擎顺序，然后从第 0 段开始请求。
    void startTranslate();
    /// 切到一个（新的）引擎时把片段进度归零，整篇重译，保证译文来源一致；
    /// 轮到搜狗且还没有会话时先走握手，握手成功再进入分段调度。
    void beginEngine();
    /// GET 搜狗首页建立会话（cookie 由 QNetworkAccessManager 自己保存）。
    void startSogouHandshake();
    void onSogouHandshakeFinished(int generation, QNetworkReply *reply);
    /// 发当前片段的请求；空片段（空行占位）直接跳过，不浪费请求次数。
    void requestCurrentChunk();
    /// 节流窗口过后真正发出 HTTP 请求（requestCurrentChunk 的延迟执行部分）。
    void sendCurrentChunk();
    /// 丢弃在途请求并让它的回复失效：用户清空/重新翻译/面板析构时都要调。
    void cancelInFlight();
    /// 一次回复到达：generation 不符就整包丢弃。
    void onReplyFinished(int generation, translate::Engine engine, QNetworkReply *reply);
    /// 当前引擎失败：还有备用就换 engine 重来，否则汇总所有原因报错（不静默失败）。
    void degradeOrFail(const QString &reason);
    void finishSuccess();
    void finishFailure();

    // ------------------------------------------------------------ 顺手操作
    void swapLanguages();
    void copyResult();
    void clearAll();
    void scheduleAutoTranslate();

    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_reply;

    QComboBox      *m_source = nullptr;
    QPushButton    *m_swap = nullptr;
    QComboBox      *m_target = nullptr;
    QComboBox      *m_engineCombo = nullptr;
    QCheckBox      *m_autoCheck = nullptr;
    QPushButton    *m_translateBtn = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLabel         *m_count = nullptr;
    QLabel         *m_status = nullptr;
    QPushButton    *m_copyBtn = nullptr;
    QPushButton    *m_clearBtn = nullptr;
    QTimer         *m_autoTimer = nullptr;

    /// 搜狗本次会话参数；面板销毁即失效，secretCode 被清空会触发下次重新握手。
    QString m_sogouSecret;
    QString m_sogouUuid;

    QList<translate::Engine> m_engineOrder;  ///< 本次翻译要依次尝试的服务
    QList<translate::Chunk>  m_chunks;       ///< 分段结果，与 m_translations 一一对应
    QStringList              m_translations;
    QStringList              m_failures;     ///< 各引擎失败原因，全部失败时汇总展示
    translate::Engine        m_engine = translate::Engine::Youdao;
    QString                  m_engineUsed;  ///< 成功时实际使用的服务名
    QString                  m_successStatus;  ///< 最近一次成功文案，复制提示超时后还原
    int                      m_statusGeneration = 0;  ///< 防止旧定时器覆盖新提示
    int                      m_enginePos = 0;
    int                      m_chunkIndex = 0;
    int                      m_detectedIndex = 1;  ///< 本地检测出的源语言，默认英语
    int                      m_generation = 0;
    qint64                   m_lastRequestMs = 0;
    bool                     m_busy = false;
};

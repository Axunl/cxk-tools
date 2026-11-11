#pragma once

// ---------------------------------------------------------------------------
// JSON 编辑器：在 QPlainTextEdit 上叠加语法高亮、键名补全、成对符号自动补齐、
// 回车自动缩进、行号栏与代码折叠。
//
// 为什么不用现成方案：QPlainTextEdit 没有 setCompleter()（那是 QLineEdit 的 API），
// 补全得自己驱动 QCompleter 的弹窗；JSON 又是多行文本，不能退化成单行输入框。
// 折叠同理：Qt 没有公开的折叠 API，得自己维护"哪些行被折起来了"。
//
// 补全候选来自当前文档里已有的键：写大段配置时最常见的就是「重复用已有的键」，
// 这比内置一份 JSON 关键字表有用得多。
//
// 折叠的做法是把区块内部的行 `QTextBlock::setVisible(false)`：隐藏行仍然留在文档里
// （块号、字符位置都不变，所以光标与行号不会错乱），只是排版与绘制时被跳过。
// 这是 QPlainTextEdit 唯一的折叠手段，Qt Creator 用的也是它。
// ---------------------------------------------------------------------------

#include <QHash>
#include <QPlainTextEdit>
#include <QRect>
#include <QVector>

class JsonHighlighter;
class JsonGutter;
class QCompleter;
class QStringListModel;
class QTimer;

class JsonEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit JsonEditor(QWidget *parent = nullptr);

    /// 用户往编辑区粘/拖进了一段外部文本（走 insertFromMimeData，覆盖 Ctrl+V、
    /// 右键粘贴、拖放三种入口）。上层据此决定"要不要立刻排版"。
    Q_SIGNALS:
    void textPasted();

public:

    /// 关掉键名补全（默认开启）。
    ///
    /// 目前面板只有这一个可编辑框，补全一直开着；保留这个开关是给"只读展示"或
    /// "把编辑器嵌进别的对话框"的场景用 —— 只读时也不该弹候选，纯干扰。
    void setCompletionEnabled(bool on);

    /// 把某一行（0 起始，与 QTextDocument 的块号一致）标成错误行；传 -1 清除。
    /// 用户一改内容就自动清除 —— 旧的行号在编辑后已经指错地方了。
    void setErrorLine(int line);

    /// 折叠 / 展开以 line 为起点的区块；该行没有可折叠的区块时返回 false。
    bool toggleFold(int line);

    /// 文档全文，**包含被折叠隐藏的行**。
    ///
    /// 读全文一律用它：`QPlainTextEdit::toPlainText()` 对隐藏块的处理没有任何文档
    /// 保证（Qt 6.8 实测会把隐藏块也带上，但不该赌这一点）。折叠只是显示状态，
    /// 格式化、复制、补全扫键看到的都必须是完整内容。
    QString fullText() const;

    /// 输入法正在拼字（拼音/五笔的 preedit 还没上屏）。
    /// 上层据此推迟"自动格式化"这类整篇重写的动作，否则会把拼字打断。
    bool isComposing() const { return m_composing; }

private:
    friend class JsonGutter;   ///< 行号栏只是个绘制/点击代理，转调下面几个私有钩子

    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void insertFromMimeData(const QMimeData *source) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    // ---- 行号栏（由 JsonGutter 转发）
    void gutterPaintEvent(QPaintEvent *event);
    void gutterMousePressEvent(QMouseEvent *event);
    void gutterMouseMoveEvent(QMouseEvent *event);
    void gutterLeaveEvent();

    void updateGutterWidth();
    void layoutGutter();
    void updateGutter(const QRect &rect, int dy);
    int gutterWidth() const;

    // ---- 折叠
    /// 可折叠区块的终点行；-1 = 该行没有可折叠的区块（含"区域信息待重算"）。
    int foldEnd(int line) const;
    /// 从当前文档重算"起点行 → 终点行"表（m_foldDirty 为假时直接返回）。
    void ensureFoldRegions();
    /// 按 m_foldStarts 重设所有块的可见性；返回是否真的改过。
    bool applyFoldVisibility();
    /// 文本变动后延迟重算：丢掉已经对不上形状的折叠，并在光标被藏起来时展开。
    void revalidateFolds();
    /// 展开包含 line 的那个折叠（line 是隐藏行才有意义）。
    void removeFoldContaining(int line);
    /// 光标所在的折叠占位符（"⋯ N 行"小药丸）的点击区。
    void paintFoldChips();

    /// 处理成对符号自动补齐；返回 true 表示按键已被消费。
    bool handleAutoPair(QKeyEvent *event);
    /// 回车自动缩进 + 行首闭括号自动退格（还有"敲 } 跳过已有 }"）；返回是否已消费按键。
    bool handleAutoIndent(QKeyEvent *event);
    /// @param force true = 用户主动唤起（Ctrl+空格），允许空前缀也弹窗
    void triggerCompletion(bool force);
    void rebuildKeys();
    void insertCompletion(const QString &completion);
    void applyErrorSelection();

    JsonHighlighter *m_highlighter = nullptr;
    QCompleter *m_completer = nullptr;
    QStringListModel *m_completionModel = nullptr;
    QTimer *m_keyTimer = nullptr;   ///< 文本高频变动时延迟重建候选，避免每敲一个字符都扫全文
    QTimer *m_foldTimer = nullptr;  ///< 同上：折叠区只在一段输入停下来后重算
    QStringList m_keys;             ///< 当前文档里的键名（补全候选缓存）
    bool m_keysDirty = true;
    bool m_completionEnabled = true;
    int m_prefixStart = 0;          ///< 弹窗打开时"待替换片段"的起点
    int m_errorLine = -1;
    bool m_composing = false;       ///< 输入法拼字中

    JsonGutter *m_gutter = nullptr;
    QHash<int, int> m_regionEnd;    ///< 起点行 → 配对的闭括号行（m_foldDirty 为真时不可信）
    QHash<int, int> m_foldStarts;   ///< 已折叠的起点行 → 折叠时的终点行
    QVector<QRect> m_chipRects;     ///< 折叠占位符的点击区（视口坐标，绘制时刷新）
    QVector<int> m_chipLines;
    bool m_foldDirty = true;
    bool m_inFoldUpdate = false;    ///< 正在改块可见性：此时 textChanged 是自己人干的
    int m_hoverLine = -1;           ///< 行号栏里鼠标悬停的行（高亮箭头用）
};

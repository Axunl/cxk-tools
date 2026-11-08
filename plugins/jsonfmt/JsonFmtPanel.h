#pragma once

// ---------------------------------------------------------------------------
// JSON 工具面板（插件内部使用，不进 api/，宿主只认 ITool）。
//
// 左输入右输出：输入框带语法高亮、键名补全、行号与折叠，并做了两件"顺手"的事：
//   1. 粘贴/拖进来的内容先格式化（整篇能解析就整篇排版，光标位置跟着内容走）；
//   2. 手动输入时自动缩进（回车按当前结构给缩进、行首闭括号自动退一级）。
//
// 刻意**不做**"边打字边整篇重排"：光标会被甩走、撤销栈会被打散，输入体验反而更差。
// 右侧是最近一次独立操作（格式化 / 压缩 / 转义 / 反转义）的结果。
// ---------------------------------------------------------------------------

#include <QWidget>

class JsonEditor;
class QLabel;

class JsonFmtPanel : public QWidget
{
    Q_OBJECT

public:
    explicit JsonFmtPanel(QWidget *parent = nullptr);

protected:
    /// 面板一显示就把焦点放进输入框（宿主换页/开独立窗口都走这里）
    void showEvent(QShowEvent *event) override;

private:
    void setStatus(const QString &text, bool ok);

    /// @param indent true = 缩进格式化，false = 压缩成一行
    void formatJson(bool indent);

    /// 粘贴进来的内容先排版：整篇能解析就整篇重排，不能解析就静默不动。
    /// 不做去抖 —— 粘贴是一次性动作，用户期望"松手就排好"。
    void formatPasted();

    /// 用替换全文的方式写回，保留撤销栈（setPlainText 会清空它）。
    void replaceInputText(const QString &text, int cursorPos);

    JsonEditor *m_input = nullptr;
    JsonEditor *m_output = nullptr;
    QLabel *m_status = nullptr;
};

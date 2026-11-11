#pragma once

// ---------------------------------------------------------------------------
// JSON 工具面板（插件内部使用，不进 api/，宿主只认 ITool）。
//
// **只有一个编辑框**：格式化 / 压缩 / 转义 / 反转义都是**原地改写**同一份内容，
// 不再另开一个只读结果框。这样"输入"和"结果"永远是同一段文本，用户不会在两个框
// 之间搞不清哪边是当前数据，也不用担心"我改完左边，右边还是旧结果"。
//
// 编辑框带语法高亮、键名补全、行号与折叠，并做了两件"顺手"的事：
//   1. 粘贴/拖进来的内容先格式化（整篇能解析就整篇排版，光标位置跟着内容走）；
//   2. 手动输入时自动缩进（回车按当前结构给缩进、行首闭括号自动退一级）。
//
// 刻意**不做**"边打字边整篇重排"：光标会被甩走、撤销栈会被打散，输入体验反而更差。
// 原地改写一律走 QTextCursor 替换全文（而不是 setPlainText），并且每次都做光标映射，
// 所以用户按 Ctrl+Z 能一步步退回去，光标也不会莫名跳到文首。
// ---------------------------------------------------------------------------

#include <QWidget>

class JsonEditor;
class QLabel;

class JsonFmtPanel : public QWidget
{
    Q_OBJECT

public:
    explicit JsonFmtPanel(QWidget *parent = nullptr);

    /// 宿主把首页搜索框里的 JSON 带进来时调用：整段放进编辑框并排版。
    /// 只做「放进去 + 格式化」，不做解析失败提示（宿主只在解析通过时才带内容）。
    void setInputText(const QString &text);

protected:
    /// 面板一显示就把焦点放进编辑框（宿主换页/开独立窗口都走这里）
    void showEvent(QShowEvent *event) override;

private:
    void setStatus(const QString &text, bool ok);

    /// @param indent true = 缩进格式化，false = 压缩成一行
    void formatJson(bool indent);

    /// 粘贴进来的内容先排版：整篇能解析就整篇重排，不能解析就静默不动。
    /// 不做去抖 —— 粘贴是一次性动作，用户期望"松手就排好"。
    /// @param okStatus 成功时的状态文案；留空用"已格式化粘贴内容"。
    void formatPasted(const QString &okStatus = QString());

    /// 把一次操作的结果**原地**写回编辑框，并给出状态回执。
    ///
    /// 结果与原文完全相同时不改写（避免白白打散撤销栈），只弹 sameStatus 文案 ——
    /// 用户点了按钮，界面必须有反应，否则会以为按钮坏了。
    ///
    /// @param newText    操作结果
    /// @param okStatus   改写成功时的提示
    /// @param sameStatus 结果与原文一致时的提示
    /// @return true = 内容确实变了
    bool applyResult(const QString &newText, const QString &okStatus, const QString &sameStatus);

    /// 用替换全文的方式写回，保留撤销栈（setPlainText 会清空它）。
    void replaceInputText(const QString &text, int cursorPos);

    JsonEditor *m_editor = nullptr;
    QLabel *m_status = nullptr;
};

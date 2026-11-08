#pragma once

// ---------------------------------------------------------------------------
// JSON 语法高亮（QSyntaxHighlighter）。
//
// 为什么手写扫描而不是一堆 QRegularExpression：键和值都是字符串，只能靠
// "闭引号后面跟不跟冒号"区分，用正则要串联两三条规则还会互相覆盖；
// 手写一遍线性扫描反而更短、更快，也顺手把"未闭合字符串"标成错误色。
//
// 只做逐行扫描：JSON 字符串不允许跨行，所以不需要跨行的状态机。
// ---------------------------------------------------------------------------

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class JsonHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit JsonHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;

private:
    QTextCharFormat m_key;
    QTextCharFormat m_string;
    QTextCharFormat m_number;
    QTextCharFormat m_literal;   ///< true / false / null
    QTextCharFormat m_punct;     ///< {} [] , :
    QTextCharFormat m_error;     ///< 未闭合字符串（JSON 里必然是错的）
};

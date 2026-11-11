#pragma once

// ---------------------------------------------------------------------------
// SQL 文本处理（纯逻辑，不 include 任何 QWidget，可单独写断言程序验证）。
//
// 三件事共用同一套词法分析：
//   1. MyBatis 日志还原：把 `Preparing:` 里那些 `?` 按 `Parameters:` 的值填回去；
//   2. SQL 格式化：子句换行 + 缩进 + 关键字大小写；
//   3. SQL 压缩：去掉注释与多余空白，压成一行。
//
// 为什么必须先做词法分析，而不是直接对字符串做正则替换：
//   * 字符串字面量里的逗号、括号、`?`、`--` 都不是语法结构，直接处理会把
//     `WHERE name = 'a,b'` 当成两列，把 `'what?'` 当成占位符；
//   * 反引号 / 方括号标识符里可能是关键字（`order` 列名），不能改大小写；
//   * 注释里的 `?` 同样不是占位符。
//   这些正是这类工具最容易翻车的地方，所以 tokenize() 是唯一的入口。
// ---------------------------------------------------------------------------

#include <QChar>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sqlfmt {

// ===========================================================================
// 词法
// ===========================================================================

/// 词法单元。pos/len 指向原文偏移 —— 还原占位符时按位置改原文，
/// 这样用户原有的大小写和排版不会被我们顺手抹掉。
struct Token
{
    enum Kind {
        Word,        ///< 标识符或关键字（含中文等 Unicode 字母）
        Number,      ///< 数字字面量（含 0x 十六进制、小数、科学计数）
        StringLit,   ///< '...' 或 "..." 字符串（保留原引号）
        QuotedIdent, ///< `...` 或 [...] 引号标识符
        LineComment, ///< -- ... 或 # ...
        BlockComment,///< /* ... */（可能跨行）
        Symbol       ///< 括号、逗号、点、运算符、占位符 ?
    };

    Kind    kind = Symbol;
    QString text;    ///< 原样文本（字符串带引号，注释带注释符）
    int     pos = 0; ///< 在原文中的起始偏移
    int     len = 0; ///< 长度，与 text.size() 一致
};

/// 能出现在标识符/关键字里的字符。@ 与 $ 是 MySQL / Oracle 的变量前缀，
/// 一起收进来可以避免 `@p` 被拆成两个 token。
inline bool isWordChar(QChar ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('$')
        || ch == QLatin1Char('@');
}

/// SQL 关键字表。只用来决定「要不要改大小写」和「括号前要不要空格」，
/// 漏掉某个关键字最多是大小写不动，不会破坏语义。
inline const QSet<QString> &keywords()
{
    static const QSet<QString> set = [] {
        static const char *words[] = {
            // 查询
            "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "NULL", "IS", "IN", "LIKE",
            "BETWEEN", "EXISTS", "ALL", "ANY", "SOME", "CASE", "WHEN", "THEN", "ELSE",
            "END", "AS", "ON", "USING", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER",
            "CROSS", "NATURAL", "UNION", "INTERSECT", "EXCEPT", "MINUS", "GROUP", "BY",
            "ORDER", "HAVING", "LIMIT", "OFFSET", "DISTINCT", "DISTINCTROW", "TOP",
            "ASC", "DESC", "NULLS", "FIRST", "LAST", "VALUES", "VALUE", "RETURNING",
            "WITH", "RECURSIVE", "OVER", "PARTITION", "ROWS", "RANGE", "PRECEDING",
            "FOLLOWING", "UNBOUNDED", "CURRENT", "ROW", "WINDOW", "FETCH", "NEXT",
            "ONLY", "TRUE", "FALSE", "UNKNOWN", "CAST", "CONVERT", "COLLATE", "ESCAPE",
            "INTERVAL", "COALESCE", "NULLIF", "GREATEST", "LEAST",
            // 写入
            "INSERT", "INTO", "UPDATE", "SET", "DELETE", "REPLACE", "IGNORE",
            "DUPLICATE", "KEY", "STRAIGHT_JOIN", "LOW_PRIORITY", "HIGH_PRIORITY",
            "DELAYED", "QUICK", "LOCK", "SHARE", "MODE", "FOR", "SKIP", "LOCKED",
            "NOWAIT", "CONFLICT", "DO", "NOTHING",
            // DDL
            "CREATE", "ALTER", "DROP", "TRUNCATE", "TABLE", "INDEX", "VIEW", "DATABASE",
            "SCHEMA", "ADD", "COLUMN", "PRIMARY", "FOREIGN", "REFERENCES", "UNIQUE",
            "DEFAULT", "AUTO_INCREMENT", "ENGINE", "CHARSET", "COMMENT", "IF", "TEMPORARY",
            "CASCADE", "RESTRICT", "CONSTRAINT", "CHECK", "MODIFY", "CHANGE", "RENAME",
            // 事务
            "BEGIN", "START", "COMMIT", "ROLLBACK", "TRANSACTION", "SAVEPOINT", "WORK",
            // 类型：格式化时一起大写，读起来更整齐
            "INT", "INTEGER", "BIGINT", "SMALLINT", "TINYINT", "DECIMAL", "NUMERIC",
            "FLOAT", "DOUBLE", "REAL", "BOOLEAN", "BIT", "CHAR", "VARCHAR", "NCHAR",
            "NVARCHAR", "TEXT", "BLOB", "DATE", "TIME", "DATETIME", "TIMESTAMP", "YEAR",
            "JSON", "UUID", "SERIAL", "UNSIGNED", "SIGNED", "ZEROFILL",
        };
        QSet<QString> result;
        for (const char *word : words)
            result.insert(QString::fromLatin1(word));
        return result;
    }();
    return set;
}

inline bool isKeyword(const QString &word)
{
    return keywords().contains(word.toUpper());
}

///
/// 把 SQL 切成 token 序列。空白与换行**不产生 token**：排版完全由格式化器决定，
/// 保留原始空白反而会让压缩/格式化两处逻辑各写一份。
///
/// 容错：字符串或注释没有闭合时，取到文本末尾并算一个 token —— 让用户看到
/// 自己漏了引号，而不是抛异常或产出半截结果。
///
inline QVector<Token> tokenize(const QString &sql)
{
    QVector<Token> out;
    const int n = sql.size();
    int i = 0;

    // 统一出口，避免每个分支重复构造 Token
    auto push = [&](Token::Kind kind, int start, int end) {
        Token t;
        t.kind = kind;
        t.pos = start;
        t.len = end - start;
        t.text = sql.mid(start, t.len);
        out.append(t);
    };

    while (i < n) {
        const QChar ch = sql.at(i);

        if (ch.isSpace()) {
            ++i;
            continue;
        }

        // ---- 行注释：`--` 与 MySQL 的 `#`
        if ((ch == QLatin1Char('-') && i + 1 < n && sql.at(i + 1) == QLatin1Char('-'))
            || ch == QLatin1Char('#')) {
            int j = i;
            while (j < n && sql.at(j) != QLatin1Char('\n'))
                ++j;
            push(Token::LineComment, i, j);
            i = j;
            continue;
        }

        // ---- 块注释：可能跨行，闭合符找不到就吃到末尾
        if (ch == QLatin1Char('/') && i + 1 < n && sql.at(i + 1) == QLatin1Char('*')) {
            const int close = sql.indexOf(QLatin1String("*/"), i + 2);
            const int j = (close < 0) ? n : close + 2;
            push(Token::BlockComment, i, j);
            i = j;
            continue;
        }

        // ---- 字符串：单/双引号，支持 '' 与反斜杠两种转义
        if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) {
            const QChar quote = ch;
            int j = i + 1;
            while (j < n) {
                if (sql.at(j) == QLatin1Char('\\') && j + 1 < n) {
                    j += 2;   // 跳过被转义的字符（含转义引号）
                    continue;
                }
                if (sql.at(j) == quote) {
                    if (j + 1 < n && sql.at(j + 1) == quote) {
                        j += 2;   // '' 表示一个字面单引号
                        continue;
                    }
                    ++j;
                    break;
                }
                ++j;
            }
            push(Token::StringLit, i, j);
            i = j;
            continue;
        }

        // ---- 反引号 / 方括号标识符
        if (ch == QLatin1Char('`') || ch == QLatin1Char('[')) {
            const QChar close = (ch == QLatin1Char('`')) ? QLatin1Char('`') : QLatin1Char(']');
            int j = i + 1;
            while (j < n) {
                if (sql.at(j) == close) {
                    // 反引号可以写成 `` 转义；方括号没有这层约定
                    if (close == QLatin1Char('`') && j + 1 < n && sql.at(j + 1) == close) {
                        j += 2;
                        continue;
                    }
                    ++j;
                    break;
                }
                ++j;
            }
            push(Token::QuotedIdent, i, j);
            i = j;
            continue;
        }

        // ---- 数字
        if (ch.isDigit()) {
            int j = i;
            if (ch == QLatin1Char('0') && j + 1 < n
                && (sql.at(j + 1) == QLatin1Char('x') || sql.at(j + 1) == QLatin1Char('X'))) {
                j += 2;
                while (j < n && (sql.at(j).isDigit()
                                 || (sql.at(j).toUpper() >= QLatin1Char('A')
                                     && sql.at(j).toUpper() <= QLatin1Char('F'))))
                    ++j;
            } else {
                while (j < n && (sql.at(j).isDigit() || sql.at(j) == QLatin1Char('.')))
                    ++j;
                if (j < n && (sql.at(j) == QLatin1Char('e') || sql.at(j) == QLatin1Char('E'))) {
                    int k = j + 1;
                    if (k < n && (sql.at(k) == QLatin1Char('+') || sql.at(k) == QLatin1Char('-')))
                        ++k;
                    if (k < n && sql.at(k).isDigit()) {
                        j = k;
                        while (j < n && sql.at(j).isDigit())
                            ++j;
                    }
                }
            }
            push(Token::Number, i, j);
            i = j;
            continue;
        }

        // ---- 标识符 / 关键字
        if (isWordChar(ch)) {
            int j = i;
            while (j < n && isWordChar(sql.at(j)))
                ++j;
            push(Token::Word, i, j);
            i = j;
            continue;
        }

        // ---- 符号：先试三字符（<=>、->>），再试两字符，最后单字符
        if (i + 2 < n && (sql.mid(i, 3) == QLatin1String("<=>") || sql.mid(i, 3) == QLatin1String("->>"))) {
            push(Token::Symbol, i, i + 3);
            i += 3;
            continue;
        }
        if (i + 1 < n) {
            static const QSet<QString> twoChar = {
                QStringLiteral("<="), QStringLiteral(">="), QStringLiteral("<>"),
                QStringLiteral("!="), QStringLiteral("||"), QStringLiteral("&&"),
                QStringLiteral(":="), QStringLiteral("::"), QStringLiteral("->"),
                QStringLiteral("<<"), QStringLiteral(">>"),
            };
            const QString two = sql.mid(i, 2);
            if (twoChar.contains(two)) {
                push(Token::Symbol, i, i + 2);
                i += 2;
                continue;
            }
        }
        push(Token::Symbol, i, i + 1);
        ++i;
    }
    return out;
}

// ===========================================================================
// 空格规则（格式化与压缩共用）
// ===========================================================================

///
/// 判断 current 前要不要插一个空格。prevIdx 是**上一个真正写进输出的** token，
/// 不是 curIdx-1：压缩时注释被丢掉，相邻关系会变。
///
/// 规则就三条：括号内侧与点号两侧不留空；函数名（非关键字的词）后紧跟括号不留空；
/// 其余一律留一个空格。一元正负号单独判一次，否则会写出 `a = - 1`。
///
inline bool needSpace(const QVector<Token> &tokens, int prevIdx, int curIdx)
{
    if (prevIdx < 0 || prevIdx >= tokens.size() || curIdx <= 0 || curIdx >= tokens.size())
        return false;

    const Token &cur = tokens.at(curIdx);
    const Token &prev = tokens.at(prevIdx);

    if (cur.kind == Token::Symbol) {
        const QString &s = cur.text;
        if (s == QLatin1String(")") || s == QLatin1String(",") || s == QLatin1String(";")
            || s == QLatin1String("."))
            return false;
        if (s == QLatin1String("(")) {
            // count( / schema.func( 不加空格；IN ( / VALUES ( / FROM ( 要加
            const bool prevIsPlainWord = prev.kind == Token::Word && !isKeyword(prev.text);
            return !prevIsPlainWord && prev.kind != Token::QuotedIdent;
        }
    }

    if (prev.kind == Token::Symbol) {
        if (prev.text == QLatin1String("(") || prev.text == QLatin1String("."))
            return false;
        // 一元正负号：前一个 token 不是值（是运算符/左括号/逗号）时视为一元
        if ((prev.text == QLatin1String("-") || prev.text == QLatin1String("+")) && prevIdx > 0) {
            const Token &before = tokens.at(prevIdx - 1);
            const bool beforeIsValue = before.kind == Token::Word || before.kind == Token::Number
                || before.kind == Token::StringLit || before.kind == Token::QuotedIdent
                || (before.kind == Token::Symbol && before.text == QLatin1String(")"));
            if (!beforeIsValue)
                return false;
        }
    }

    return true;
}

// ===========================================================================
// 格式化
// ===========================================================================

struct FormatOptions
{
    bool upperKeywords = true;   ///< 关键字大写（关掉则小写）；标识符大小写从不改动
    int  indentWidth = 2;        ///< 每层缩进空格数
    bool oneColumnPerLine = false; ///< SELECT 的每一列单独一行
};

///
/// 把 tokens 从 i 起的一段识别成"子句起点"（含 GROUP BY / LEFT OUTER JOIN 这类多词组合）。
/// 命中返回规范化文本（大写、单词间一个空格）并告知吃掉了几个 token，否则返回空串。
///
/// 只认列在表里的组合：宁可少换行，也不要把 `order` 这种列名当成 ORDER BY。
///
inline QString matchClause(const QVector<Token> &tokens, int i, int *consumed)
{
    *consumed = 0;
    if (i < 0 || i >= tokens.size() || tokens.at(i).kind != Token::Word)
        return QString();

    // 长的写在前面：先匹配 LEFT OUTER JOIN 再匹配 LEFT JOIN，否则前者永远吃不到
    static const QVector<QVector<QString>> phrases = [] {
        const char *raw[][3] = {
            {"LEFT", "OUTER", "JOIN"}, {"RIGHT", "OUTER", "JOIN"}, {"FULL", "OUTER", "JOIN"},
            {"NATURAL", "LEFT", "JOIN"}, {"LEFT", "OUTER", "JOIN"},
            {"SELECT", "", ""}, {"FROM", "", ""}, {"WHERE", "", ""},
            {"GROUP", "BY", ""}, {"ORDER", "BY", ""}, {"HAVING", "", ""},
            {"LIMIT", "", ""}, {"OFFSET", "", ""}, {"FETCH", "", ""},
            {"UNION", "ALL", ""}, {"UNION", "", ""}, {"INTERSECT", "", ""},
            {"EXCEPT", "", ""}, {"MINUS", "", ""},
            {"INSERT", "INTO", ""}, {"INSERT", "", ""}, {"REPLACE", "INTO", ""},
            {"UPDATE", "", ""}, {"SET", "", ""}, {"VALUES", "", ""}, {"VALUE", "", ""},
            {"DELETE", "FROM", ""}, {"DELETE", "", ""}, {"RETURNING", "", ""},
            {"WITH", "", ""}, {"WINDOW", "", ""},
            {"INNER", "JOIN", ""}, {"LEFT", "JOIN", ""}, {"RIGHT", "JOIN", ""},
            {"FULL", "JOIN", ""}, {"CROSS", "JOIN", ""}, {"NATURAL", "JOIN", ""},
            {"JOIN", "", ""}, {"ON", "", ""}, {"USING", "", ""},
        };
        QVector<QVector<QString>> table;
        for (const auto &row : raw) {
            QVector<QString> phrase;
            for (const char *word : row) {
                if (!*word)
                    continue;
                phrase.append(QString::fromLatin1(word));
            }
            if (!phrase.isEmpty())
                table.append(phrase);
        }
        return table;
    }();

    // 收集从 i 起连续（跳过注释）的前三个单词，用于比对
    QStringList words;
    QVector<int> indexes;
    for (int k = i; k < tokens.size() && words.size() < 3; ++k) {
        const Token &t = tokens.at(k);
        if (t.kind == Token::LineComment || t.kind == Token::BlockComment)
            continue;
        if (t.kind != Token::Word)
            break;
        words.append(t.text.toUpper());
        indexes.append(k);
    }

    for (const QVector<QString> &phrase : phrases) {
        if (phrase.size() > words.size())
            continue;
        bool hit = true;
        for (int k = 0; k < phrase.size(); ++k) {
            if (words.at(k) != phrase.at(k)) {
                hit = false;
                break;
            }
        }
        if (!hit)
            continue;
        *consumed = indexes.at(phrase.size() - 1) - i + 1;
        return QStringList(phrase.begin(), phrase.end()).join(QLatin1Char(' '));
    }
    return QString();
}

///
/// 格式化。风格与主流 SQL 工具一致：
///
///   SELECT a, b            -- 子句顶格，每列不换行（oneColumnPerLine 打开则每列一行）
///   FROM t                 -- 子查询里的内容整体缩进一层
///   WHERE a = 1            -- AND / OR 缩进一层，读条件时眼睛不用来回扫
///     AND b = 2
///
/// 括号里如果不是子查询（函数调用、IN 列表、VALUES 元组）就不换行，
/// 否则 `count(*)` 会被拆成三行 —— 那不是"更好读"，那是噪音。
///
inline QString formatSql(const QString &sql, const FormatOptions &options)
{
    const QVector<Token> tokens = tokenize(sql);
    if (tokens.isEmpty())
        return QString();

    const int indentWidth = qBound(1, options.indentWidth, 8);

    QString out;
    int     lineLevel = 0;    ///< 当前行的缩进层级
    bool    lineStart = true; ///< 本行还没有写过任何字符
    int     baseLevel = 0;    ///< 子句对齐层级（进子查询 +1，出子查询还原）

    bool inSelectList = false; ///< 是否处在 SELECT 列表（oneColumnPerLine 时才用）
    int  selectListLevel = 0;
    int  selectListDepth = 0;  ///< 进入列表时的括号深度，用来忽略函数内部的逗号

    bool afterBetween = false; ///< BETWEEN x AND y 里的 AND 不能当成条件连接词换行

    struct ParenState
    {
        bool subquery = false;
        int  base = 0;
    };
    QVector<ParenState> parens;

    int lastEmitted = -1; ///< 上一个写进输出的 token，空格规则要用

    // 换行并设定下一行的缩进层级。已经在行首时不再补 '\n'，
    // 否则连续的换行请求会造出一串空行。
    auto newline = [&](int level) {
        while (out.endsWith(QLatin1Char(' ')))
            out.chop(1);
        if (!out.isEmpty() && !lineStart)
            out += QLatin1Char('\n');
        lineLevel = qMax(0, level);
        lineStart = true;
    };

    auto put = [&](const QString &text, bool space) {
        if (lineStart) {
            out += QString(lineLevel * indentWidth, QLatin1Char(' '));
            lineStart = false;
        } else if (space) {
            out += QLatin1Char(' ');
        }
        out += text;
    };

    // 关键字按选项统一大小写；标识符原样输出（改列名大小写是要出事的）
    auto render = [&](const Token &token) -> QString {
        if (token.kind != Token::Word || !isKeyword(token.text))
            return token.text;
        return options.upperKeywords ? token.text.toUpper() : token.text.toLower();
    };

    for (int i = 0; i < tokens.size(); ++i) {
        const Token &t = tokens.at(i);

        // ---- 注释
        if (t.kind == Token::LineComment) {
            newline(baseLevel);
            put(t.text, false);
            lastEmitted = i;
            newline(baseLevel);   // 行注释后面的内容必须换行，否则会被注释吃掉
            continue;
        }
        if (t.kind == Token::BlockComment) {
            put(t.text, !lineStart && needSpace(tokens, lastEmitted, i));
            lastEmitted = i;
            continue;
        }

        // ---- AND / OR：条件连接词，缩进一层；括号内与 BETWEEN 里不换行
        if (t.kind == Token::Word) {
            const QString upper = t.text.toUpper();
            if (upper == QLatin1String("AND") || upper == QLatin1String("OR")) {
                if (afterBetween || !parens.isEmpty()) {
                    put(render(t), !lineStart && needSpace(tokens, lastEmitted, i));
                } else {
                    newline(baseLevel + 1);
                    put(render(t), false);
                }
                afterBetween = false;
                lastEmitted = i;
                continue;
            }
        }

        // ---- 子句起点
        if (t.kind == Token::Word) {
            int consumed = 0;
            const QString clause = matchClause(tokens, i, &consumed);
            if (!clause.isEmpty()) {
                const bool isOn = (clause == QLatin1String("ON") || clause == QLatin1String("USING"));
                newline(isOn ? baseLevel + 1 : baseLevel);
                put(options.upperKeywords ? clause : clause.toLower(), false);

                inSelectList = false;
                if (clause == QLatin1String("SELECT") && options.oneColumnPerLine) {
                    newline(baseLevel + 1);
                    inSelectList = true;
                    selectListLevel = baseLevel + 1;
                    selectListDepth = parens.size();
                }

                lastEmitted = i + consumed - 1;
                i += consumed - 1;   // 多词子句的后续单词已经一起输出了
                continue;
            }
        }

        // ---- 括号
        if (t.kind == Token::Symbol && t.text == QLatin1String("(")) {
            // 往后看一个有效 token：SELECT / WITH 开头才算子查询，要换行缩进
            bool subquery = false;
            for (int k = i + 1; k < tokens.size(); ++k) {
                const Token &next = tokens.at(k);
                if (next.kind == Token::LineComment || next.kind == Token::BlockComment)
                    continue;
                if (next.kind == Token::Word) {
                    const QString upper = next.text.toUpper();
                    subquery = (upper == QLatin1String("SELECT") || upper == QLatin1String("WITH"));
                }
                break;
            }
            put(t.text, !lineStart && needSpace(tokens, lastEmitted, i));
            parens.append({subquery, baseLevel});
            if (subquery) {
                baseLevel += 1;
                newline(baseLevel);
            }
            lastEmitted = i;
            continue;
        }
        if (t.kind == Token::Symbol && t.text == QLatin1String(")")) {
            bool subquery = false;
            if (!parens.isEmpty()) {
                const ParenState state = parens.takeLast();
                subquery = state.subquery;
                if (subquery) {
                    baseLevel = qMax(0, state.base);
                    inSelectList = false;
                }
            }
            if (subquery)
                newline(baseLevel);
            put(t.text, false);
            lastEmitted = i;
            continue;
        }

        // ---- 逗号：只有顶层 SELECT 列表的逗号才换行
        if (t.kind == Token::Symbol && t.text == QLatin1String(",")) {
            put(t.text, false);
            if (inSelectList && parens.size() == selectListDepth)
                newline(selectListLevel);
            lastEmitted = i;
            continue;
        }

        // ---- 分号：一条语句结束，回到顶格
        if (t.kind == Token::Symbol && t.text == QLatin1String(";")) {
            put(t.text, false);
            inSelectList = false;
            newline(0);
            lastEmitted = i;
            continue;
        }

        // ---- 其余（词、数字、字符串、引号标识符、运算符）
        put(render(t), !lineStart && needSpace(tokens, lastEmitted, i));
        if (t.kind == Token::Word && t.text.compare(QLatin1String("between"), Qt::CaseInsensitive) == 0)
            afterBetween = true;
        lastEmitted = i;
    }

    while (out.endsWith(QLatin1Char(' ')) || out.endsWith(QLatin1Char('\n')))
        out.chop(1);
    return out;
}

// ===========================================================================
// 压缩
// ===========================================================================

///
/// 压成一行：去掉注释，所有换行/连续空白折成一个空格，保留字符串内部原样。
/// 关键字大小写不动（压缩只改排版，不替用户做决定），但标点两侧的空格会收紧。
///
inline QString compressSql(const QString &sql)
{
    const QVector<Token> tokens = tokenize(sql);
    QString out;
    int lastEmitted = -1;

    for (int i = 0; i < tokens.size(); ++i) {
        const Token &t = tokens.at(i);
        if (t.kind == Token::LineComment || t.kind == Token::BlockComment)
            continue;   // 注释在压缩结果里没有位置

        if (!out.isEmpty() && needSpace(tokens, lastEmitted, i))
            out += QLatin1Char(' ');
        out += t.text;
        lastEmitted = i;
    }
    return out;
}

// ===========================================================================
// MyBatis 日志还原
// ===========================================================================

/// 一个参数：`N(String)` → value = "N"、type = "String"；`null` 没有类型。
struct Param
{
    QString value;
    QString type;
};

///
/// 解析 `Parameters:` 后面的那一串。
///
/// 难点是值里本身可能带 ", "（字符串参数），不能简单 split。做法是按 ", " 切开后
/// 累积，直到累积串以 `(类型)` 结尾为止 —— MyBatis 总是把类型写在参数末尾，
/// 所以"以右括号结尾"就是一条参数的终点。
///
inline QVector<Param> parseParams(const QString &line)
{
    QVector<Param> out;
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return out;

    const QStringList segments = trimmed.split(QLatin1String(", "));
    QString buffer;
    for (const QString &segment : segments) {
        buffer += buffer.isEmpty() ? segment : QStringLiteral(", ") + segment;
        if (!buffer.endsWith(QLatin1Char(')')))
            continue;   // 还没到类型括注，说明这个参数的值里含 ", "

        const int open = buffer.lastIndexOf(QLatin1Char('('));
        if (open < 0) {
            Param p;
            p.value = buffer.trimmed();
            out.append(p);
            buffer.clear();
            continue;
        }
        Param p;
        p.type = buffer.mid(open + 1, buffer.size() - open - 2).trimmed();
        p.value = buffer.left(open).trimmed();
        out.append(p);
        buffer.clear();
    }
    if (!buffer.isEmpty()) {
        // 末尾这段没有类型括注（日志被截断）：按值处理，交给上层给提示
        Param p;
        p.value = buffer.trimmed();
        out.append(p);
    }
    return out;
}

/// 把参数值转成 SQL 字面量。类型决定要不要加引号，值本身决定 NULL / 布尔。
inline QString paramLiteral(const Param &param)
{
    const QString value = param.value;

    // MyBatis 对 null 一律打印 `null`（有些驱动写成 "null(Type)"）
    if (value.compare(QLatin1String("null"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("NULL");

    const QString type = param.type.toUpper();

    static const QSet<QString> numberTypes = {
        QStringLiteral("INT"), QStringLiteral("INTEGER"), QStringLiteral("LONG"),
        QStringLiteral("SHORT"), QStringLiteral("BYTE"), QStringLiteral("DOUBLE"),
        QStringLiteral("FLOAT"), QStringLiteral("BIGDECIMAL"), QStringLiteral("BIGINTEGER"),
        QStringLiteral("DECIMAL"), QStringLiteral("NUMBER"), QStringLiteral("NUMERIC"),
        QStringLiteral("REAL"),
    };
    static const QSet<QString> boolTypes = {
        QStringLiteral("BOOLEAN"), QStringLiteral("BIT"),
    };

    if (numberTypes.contains(type))
        return value;   // 数值原样；1.0E+2 这种写法数据库也认

    if (boolTypes.contains(type)) {
        const QString lower = value.toLower();
        if (lower == QLatin1String("true") || lower == QLatin1String("1"))
            return QStringLiteral("TRUE");
        if (lower == QLatin1String("false") || lower == QLatin1String("0"))
            return QStringLiteral("FALSE");
        return value;
    }

    if (type.isEmpty()) {
        // 没类型信息时按内容猜：能解析成数字就原样，否则当字符串
        bool isNumber = false;
        value.toDouble(&isNumber);
        if (isNumber)
            return value;
    }

    // 其余（String / Date / Timestamp / LocalDateTime / UUID / byte[] …）都当字符串字面量：
    // 日期时间在 SQL 里本来就要引号，这样写 MySQL 与 PostgreSQL 都认。
    if (value.size() >= 2 && value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))
        return value;   // 日志里已经带引号，别套两层

    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));   // 单引号翻倍，标准转义
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

struct MyBatisResult
{
    QString     sql;           ///< 还原后的 SQL（多条语句用 ";\n\n" 连接）
    int         statements = 0;///< 成功还原的语句条数
    int         placeholders = 0;
    int         replaced = 0;
    QStringList warnings;      ///< 非致命问题：占位符与参数数量不一致等
};

/// 在一条 SQL 里按位置把 `?` 换成字面量。用 token 定位，字符串与注释里的 `?` 不动。
inline QString replacePlaceholders(const QString &sql, const QString &paramLine,
                                   int *placeholderCount, int *replacedCount, QStringList *warnings)
{
    const QVector<Token> tokens = tokenize(sql);
    QVector<int> marks;
    for (const Token &t : tokens) {
        if (t.kind == Token::Symbol && t.text == QLatin1String("?"))
            marks.append(t.pos);
    }
    *placeholderCount = marks.size();

    const QVector<Param> params = parseParams(paramLine);
    const int usable = qMin(marks.size(), params.size());
    *replacedCount = usable;

    if (marks.size() != params.size() && warnings) {
        warnings->append(QStringLiteral("SQL 里有 %1 个占位符，参数行却解析出 %2 个（已按较少的那个还原）")
                             .arg(marks.size())
                             .arg(params.size()));
    }
    // 从后往前替换：前面被替换成长度不同的字面量不会影响后面的偏移
    QString result = sql;
    for (int k = usable - 1; k >= 0; --k)
        result.replace(marks.at(k), 1, paramLiteral(params.at(k)));
    return result;
}

///
/// 从一整段 MyBatis 日志里抽出 `Preparing:` / `Parameters:` 配对并还原。
///
/// 支持一次粘多条语句：按出现顺序配对。也兼容"只有两行裸文本"的粘贴
/// （第一行 SQL、第二行参数），方便从别的工具搬过来。
///
inline MyBatisResult restoreMyBatisLog(const QString &log)
{
    MyBatisResult result;

    struct Pair
    {
        QString sql;
        QString params;
        bool    hasParams = false;
    };
    QVector<Pair> pairs;

    const QStringList lines = log.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;

        int index = line.indexOf(QLatin1String("Preparing:"));
        if (index >= 0) {
            Pair pair;
            pair.sql = line.mid(index + 10).trimmed();
            pairs.append(pair);
            continue;
        }
        index = line.indexOf(QLatin1String("Parameters:"));
        if (index >= 0) {
            const QString values = line.mid(index + 11).trimmed();
            if (!pairs.isEmpty() && !pairs.last().hasParams) {
                pairs.last().params = values;
                pairs.last().hasParams = true;
            } else {
                // 只有参数没有 SQL：日志被截断了，先记下来，后面统一提示
                Pair pair;
                pair.params = values;
                pair.hasParams = true;
                pairs.append(pair);
            }
            continue;
        }
    }

    // 没有日志标记：允许"第一行 SQL + 第二行参数"的裸粘贴
    if (pairs.isEmpty()) {
        QStringList nonEmpty;
        for (const QString &raw : lines) {
            if (!raw.trimmed().isEmpty())
                nonEmpty.append(raw.trimmed());
        }
        if (nonEmpty.size() == 2 && nonEmpty.first().contains(QLatin1Char('?'))) {
            Pair pair;
            pair.sql = nonEmpty.at(0);
            pair.params = nonEmpty.at(1);
            pair.hasParams = true;
            pairs.append(pair);
        } else if (nonEmpty.size() == 1 && nonEmpty.first().contains(QLatin1Char('?'))) {
            Pair pair;
            pair.sql = nonEmpty.first();
            pairs.append(pair);
            result.warnings.append(QStringLiteral("没找到 Parameters 行，只能原样输出带 ? 的 SQL"));
        } else {
            result.warnings.append(QStringLiteral(
                "没找到 `Preparing:` / `Parameters:` 行。请把 MyBatis 日志里这两行一起粘进来"));
            return result;
        }
    }

    QStringList statements;
    for (const Pair &pair : pairs) {
        if (pair.sql.isEmpty()) {
            result.warnings.append(QStringLiteral("有一段参数找不到对应的 Preparing 行，已跳过"));
            continue;
        }
        if (!pair.hasParams) {
            result.warnings.append(QStringLiteral("有一条 SQL 没找到 Parameters 行，占位符保持原样"));
        }

        int placeholders = 0;
        int replaced = 0;
        const QString restored = replacePlaceholders(pair.sql, pair.params, &placeholders, &replaced,
                                                     &result.warnings);

        result.placeholders += placeholders;
        result.replaced += replaced;
        ++result.statements;

        QString statement = restored.trimmed();
        while (statement.endsWith(QLatin1Char(';')))
            statement.chop(1);
        statements.append(statement);
    }

    if (statements.size() > 1)
        result.sql = statements.join(QStringLiteral(";\n\n")) + QLatin1Char(';');
    else if (statements.size() == 1)
        result.sql = statements.first();
    return result;
}

} // namespace sqlfmt

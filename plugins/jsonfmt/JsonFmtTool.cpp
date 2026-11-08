#include "JsonFmtTool.h"

#include "JsonText.h"   // 转义/行列换算等纯逻辑（可单独测试）

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

///
/// 工具面板：左侧输入、右侧只读输出，中间可拖动分隔。
///
class JsonFmtPanel : public QWidget
{
    Q_OBJECT

public:
    explicit JsonFmtPanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        // 左右 28px 与主窗口页头对齐
        layout->setContentsMargins(28, 0, 28, 20);
        layout->setSpacing(10);

        // ---- 工具栏
        auto *toolbar = new QHBoxLayout;
        toolbar->setSpacing(8);

        auto *formatBtn = new QPushButton(QStringLiteral("格式化"), this);
        formatBtn->setProperty("accent", true);   // 主操作，用强调色
        auto *compactBtn = new QPushButton(QStringLiteral("压缩"), this);
        auto *escapeBtn = new QPushButton(QStringLiteral("转义"), this);
        auto *unescapeBtn = new QPushButton(QStringLiteral("反转义"), this);
        auto *copyBtn = new QPushButton(QStringLiteral("复制结果"), this);
        auto *clearBtn = new QPushButton(QStringLiteral("清空"), this);

        m_status = new QLabel(this);
        m_status->setStyleSheet(QStringLiteral("font-size:12px;color:#94A3B8;"));

        toolbar->addWidget(formatBtn);
        toolbar->addWidget(compactBtn);
        toolbar->addWidget(escapeBtn);
        toolbar->addWidget(unescapeBtn);
        toolbar->addWidget(copyBtn);
        toolbar->addWidget(clearBtn);
        toolbar->addStretch(1);
        toolbar->addWidget(m_status);
        layout->addLayout(toolbar);

        // ---- 左右分栏
        auto *splitter = new QSplitter(Qt::Horizontal, this);

        m_input = new QPlainTextEdit(splitter);
        m_input->setPlaceholderText(QStringLiteral("在这里粘贴 JSON 或普通文本…"));

        m_output = new QPlainTextEdit(splitter);
        m_output->setReadOnly(true);
        m_output->setPlaceholderText(QStringLiteral("结果会出现在这里"));

        splitter->addWidget(m_input);
        splitter->addWidget(m_output);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({500, 500});
        layout->addWidget(splitter, 1);

        connect(formatBtn, &QPushButton::clicked, this, [this] { formatJson(true); });
        connect(compactBtn, &QPushButton::clicked, this, [this] { formatJson(false); });
        connect(escapeBtn, &QPushButton::clicked, this, [this] {
            m_output->setPlainText(jsontext::escapeText(m_input->toPlainText()));
            setStatus(QStringLiteral("已转义"), true);
        });
        connect(unescapeBtn, &QPushButton::clicked, this, [this] {
            m_output->setPlainText(jsontext::unescapeText(m_input->toPlainText()));
            setStatus(QStringLiteral("已反转义"), true);
        });
        connect(copyBtn, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(m_output->toPlainText());
            setStatus(QStringLiteral("已复制到剪贴板"), true);
        });
        connect(clearBtn, &QPushButton::clicked, this, [this] {
            m_input->clear();
            m_output->clear();
            setStatus(QString(), true);
        });
    }

private:
    void setStatus(const QString &text, bool ok)
    {
        m_status->setText(text);
        m_status->setStyleSheet(QStringLiteral("font-size:12px;color:%1;")
                                    .arg(ok ? QStringLiteral("#10B981") : QStringLiteral("#DC2626")));
    }

    /// @param indent true = 缩进格式化，false = 压缩成一行
    void formatJson(bool indent)
    {
        const QString text = m_input->toPlainText();
        if (text.trimmed().isEmpty()) {
            setStatus(QStringLiteral("先粘贴点 JSON 内容"), false);
            return;
        }

        QJsonParseError err{};
        // 走 UTF-8：JSON 规范就是 UTF-8，QString::toUtf8 不会丢内容
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            setStatus(QStringLiteral("解析失败：%1").arg(jsontext::describeParseError(err, text)), false);
            return;
        }

        const QByteArray out = doc.toJson(indent ? QJsonDocument::Indented : QJsonDocument::Compact);
        m_output->setPlainText(QString::fromUtf8(out));
        setStatus(indent ? QStringLiteral("已格式化") : QStringLiteral("已压缩"), true);
    }

    QPlainTextEdit *m_input = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLabel *m_status = nullptr;
};

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo JsonFmtPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("jsonfmt");
    info.name = QStringLiteral("JSON 格式化");
    // 关键词决定搜索命中：英文、中文、常见动作词都放进来
    info.keywords = QStringList{QStringLiteral("json"), QStringLiteral("format"),
                                QStringLiteral("格式化"), QStringLiteral("压缩")};
    info.description = QStringLiteral("格式化 / 压缩 / 转义 JSON 文本");
    return info;
}

QWidget *JsonFmtPlugin::createPanel(QWidget *parent)
{
    return new JsonFmtPanel(parent);
}

// .cpp 里有带 Q_OBJECT 的类（JsonFmtPanel），必须把 moc 产物包含进来
#include "JsonFmtTool.moc"

#include "CalcTool.h"

#include "ExprEval.h"      // 纯求值逻辑，住在 src/api（宿主首页的就地求值也用它）
#include "ThemePalette.h"
#include "LayoutMetrics.h"   // 颜色令牌：宿主与插件共用，深色主题下才不会留白底

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

///
/// 工具面板：表达式输入 + 大号结果 + 历史列表。
///
class CalcPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CalcPanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        // 左右 28px 与主窗口页头对齐，视觉上像同一栏
        layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
        layout->setSpacing(10);

        m_expr = new QLineEdit(this);
        m_expr->setPlaceholderText(QStringLiteral("输入表达式，回车记录，例如 (12+8)*3^2"));
        m_expr->setFixedHeight(42);
        m_expr->setStyleSheet(QStringLiteral("font-size:16px;"));   // 输入区要显眼
        layout->addWidget(m_expr);

        m_result = new QLabel(QStringLiteral("0"), this);
        m_result->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(m_result);

        m_hint = new QLabel(this);
        layout->addWidget(m_hint);

        m_history = new QListWidget(this);
        // 用 applyStyle 而不是 setStyleSheet：模板会被记在控件上，换肤时宿主
        // 能重算 @token，否则深色主题下这块列表会保持白底。
        xtools::theme::applyStyle(
            m_history,
            QStringLiteral("QListWidget{background:@bg;border:1px solid @border;border-radius:8px;}"
                           "QListWidget::item{padding:6px 10px;border-bottom:1px solid @border-subtle;"
                           "color:@text-body;}"));
        layout->addWidget(m_history, 1);

        showIdleState();

        connect(m_expr, &QLineEdit::textChanged, this, &CalcPanel::liveEvaluate);
        connect(m_expr, &QLineEdit::returnPressed, this, &CalcPanel::commit);

        // 点历史项 = 把原表达式填回输入框，方便改一个数字再算
        connect(m_history, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            const QString expr = item->data(Qt::UserRole).toString();
            if (!expr.isEmpty()) {
                m_expr->setText(expr);
                m_expr->setFocus();
            }
        });
    }

private:
    /// 结果与提示的字号/颜色集中在这里，避免三处重复写字面量。
    /// 传令牌而不是色值：换肤时宿主按令牌重算，颜色跟着主题走。
    void applyResultStyle(xtools::theme::Color color)
    {
        xtools::theme::applyStyle(m_result, QStringLiteral("font-size:30px;font-weight:600;color:@%1;")
                                                .arg(xtools::theme::tokenName(color)));
    }

    void applyHintStyle(xtools::theme::Color color)
    {
        xtools::theme::applyStyle(m_hint, QStringLiteral("font-size:12px;color:@%1;")
                                                .arg(xtools::theme::tokenName(color)));
    }

    /// 初始态（空输入）：显示 0 与操作说明。
    void showIdleState()
    {
        m_result->setText(QStringLiteral("0"));
        applyResultStyle(xtools::theme::Color::Text);
        m_hint->setText(QStringLiteral("支持 + - * / % ^ 和括号，常量 pi / e。回车把结果存入下方历史。"));
        applyHintStyle(xtools::theme::Color::TextFaint);
    }

    /// 边输边算：合法就显示结果，不合法只显示提示，不打断输入。
    void liveEvaluate()
    {
        const QString text = m_expr->text().trimmed();
        if (text.isEmpty()) {
            showIdleState();
            return;
        }

        double value = 0;
        QString error;
        if (calc::ExprEval(text).evaluate(&value, &error)) {
            m_result->setText(calc::formatNumber(value));
            applyResultStyle(xtools::theme::Color::Text);
            m_hint->setText(QStringLiteral("回车记录到历史"));
            applyHintStyle(xtools::theme::Color::Success);
        } else {
            // 用破折号而不是保留上次的结果，避免用户把旧值当成新结果
            m_result->setText(QStringLiteral("—"));
            applyResultStyle(xtools::theme::Color::TextDisabled);
            m_hint->setText(error);
            applyHintStyle(xtools::theme::Color::Danger);
        }
    }

    void commit()
    {
        const QString text = m_expr->text().trimmed();
        if (text.isEmpty())
            return;

        double value = 0;
        QString error;
        if (!calc::ExprEval(text).evaluate(&value, &error))
            return;   // 算错了就不进历史，避免污染

        auto *item = new QListWidgetItem(QStringLiteral("%1 = %2").arg(text, calc::formatNumber(value)),
                                        m_history);
        item->setData(Qt::UserRole, text);
        m_history->insertItem(0, item);   // 新的在最上面
        m_history->setCurrentRow(0);
        m_expr->selectAll();              // 全选，直接输入即替换
    }

    QLineEdit *m_expr = nullptr;
    QLabel *m_result = nullptr;
    QLabel *m_hint = nullptr;
    QListWidget *m_history = nullptr;
};

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo CalcPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("calc");
    info.name = QStringLiteral("计算器");
    info.keywords = QStringList{QStringLiteral("calc"), QStringLiteral("计算"),
                                QStringLiteral("计算器"), QStringLiteral("=")};
    info.description = QStringLiteral("表达式求值，边输边算");
    return info;
}

QWidget *CalcPlugin::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存面板，每次进入工具都会调用这里
    return new CalcPanel(parent);
}

// .cpp 里有带 Q_OBJECT 的类（CalcPanel），需要 moc 产物
#include "CalcTool.moc"

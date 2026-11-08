#pragma once

// ---------------------------------------------------------------------------
// Cron 表达式工具面板（插件内部，宿主只认 ITool）。
//
// 布局自上而下：
//   表达式 + 复制 → 中文解读 → 状态
//   → [左] 可视化选择每个字段（字段 chips + 五模式编辑器）
//     [右] 接下来 10 次运行（起算时间 + 相对时间）
//   → 快速预设 → 含秒字段开关。
//
// 双向同步约定（避免回环的关键：改哪边都收敛到「顶部表达式」再刷新）：
//   * 改表达式 → 解析成功后 classifyField 回填编辑器状态并同步「含秒字段」勾选
//     （5 段 / 6 段跟着表达式走），QSignalBlocker 防止回填再触发编辑信号；
//   * 改可视化编辑器 → buildFieldRaw 逐字段拼出表达式回写顶部
//     （不回填编辑器，保留用户当前操作）；
//   * 解读与运行时间永远从顶部表达式算出来，不存在两份事实源。
//
// 面板类放头文件是为了按 AGENTS.md 规则 2 用临时工程直接断言（开 AUTOMOC 即可），
// 与 TranslatePanel 同一处理方式。
// ---------------------------------------------------------------------------

#include <QWidget>

#include "CronExpr.h" // FieldEdit 是编辑器的状态载体，直接复用纯逻辑结构

class QButtonGroup;
class QCheckBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QGridLayout;

class CronPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CronPanel(QWidget *parent = nullptr);

private:
    void buildUi();
    /// 把表达式写进顶部输入框；syncFields 为真时按解析结果回填编辑器状态。
    void applyExpr(const QString &expr, bool syncFields);
    /// 按当前表达式重算解读 / 运行时间 / 状态 / 字段 chips。
    void refresh();
    /// 解析结果回填 6 份编辑状态（classifyField）并重载当前字段的编辑区。
    void syncFieldsFrom(const cron::Spec &spec);
    void setSecondsVisible(bool on);
    /// 状态条两态：state 取 "ok" / "error"（对应 style.qss 的属性选择器）。
    void setStatus(const QString &text, const char *state);

    /// 切换正在编辑的字段：重载编辑区控件与标题。
    void selectField(int index);
    /// m_fields[当前字段] → 编辑区控件（内部阻塞信号）。值网格只在字段变化时重建。
    void loadEditorFromState();
    /// 重建值选择网格（各字段取值范围不同；dow 只给 0~6，7 与 0 同义不重复出）。
    void rebuildValueGrid();
    /// 值网格勾选 → m_fields[当前字段].values → 重建表达式（单击/全选/清空共用）。
    void syncValuesFromGrid();
    /// 按 6 份编辑状态拼出表达式写回顶部并刷新（编辑器改动的统一出口）。
    void rebuildExpr();
    /// 同步顶部 chips 的字段名 + 当前原文（双行）。
    void updateChips(const cron::Spec &spec);

    void onExprEdited();
    void onSecondsToggled();
    void onCopy();

    // ---- 顶部
    QLineEdit *m_expr = nullptr;
    QPushButton *m_copy = nullptr;
    QLabel *m_desc = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_withSeconds = nullptr;

    // ---- 左：可视化字段编辑
    QButtonGroup *m_fieldChips = nullptr;  ///< 字段选择 chips（互斥）
    QVector<QPushButton *> m_chipBtns;     ///< 与 FieldType 顺序一致（秒那颗默认隐藏）
    QLabel *m_fieldTitle = nullptr;
    QButtonGroup *m_kindGroup = nullptr;   ///< 五种模式（互斥，id 对齐 FieldMode）
    QStackedWidget *m_kindStack = nullptr;
    QLabel *m_everyHint = nullptr;         ///< 每个：说明文案
    QSpinBox *m_periodFrom = nullptr;      ///< 周期：起点
    QSpinBox *m_periodStep = nullptr;      ///< 周期：步长
    QLabel *m_periodUnitA = nullptr;
    QLabel *m_periodUnitB = nullptr;
    QSpinBox *m_rangeFrom = nullptr;       ///< 范围：起点
    QSpinBox *m_rangeTo = nullptr;         ///< 范围：终点
    QLabel *m_valuesCount = nullptr;       ///< 指定具体值：已选个数
    QGridLayout *m_valuesGrid = nullptr;   ///< 值网格（10 列）
    QWidget *m_valuesHost = nullptr;       ///< 网格宿主（放进滚动区）
    QVector<QPushButton *> m_valueBtns;    ///< 当前字段的可选值按钮
    QLineEdit *m_customEdit = nullptr;     ///< 自定义：直写字段原文
    cron::FieldEdit m_fields[6];           ///< 6 份编辑状态，下标对齐 FieldType
    int m_current = 1;                     ///< 当前编辑的字段下标（0=秒 … 5=周，默认「分」）
    int m_gridField = -1;                  ///< 值网格已为哪个字段建好（-1 = 未建）

    // ---- 右：接下来 10 次运行
    QDateTimeEdit *m_from = nullptr;
    QPushButton *m_nowBtn = nullptr;
    QListWidget *m_next = nullptr;
};

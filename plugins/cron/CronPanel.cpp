#include "CronPanel.h"

#include "CronExpr.h" // 纯逻辑（可单独测试，见 AGENTS.md 的验证流程）

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTimeEdit>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

namespace {

/// 常用预设：覆盖最常见的 crontab 场景。点按钮 = 填表达式 + 回填编辑器。
/// 全部是 5 段标准 cron；勾选「含秒字段」时由 onSecondsToggled 统一补 "0" 秒段。
struct Preset
{
    const char *label;
    const char *expr;
};

const Preset kPresets[] = {
    {"每分钟", "* * * * *"},
    {"每 5 分钟", "*/5 * * * *"},
    {"每小时整点", "0 * * * *"},
    {"每天 0 点", "0 0 * * *"},
    {"每天 9 点", "0 9 * * *"},
    {"工作日 9 点", "0 9 * * 1-5"},
    {"每周一 9 点", "0 9 * * 1"},
    {"每月 1 号", "0 0 1 * *"},
};

/// 五种编辑模式的单选标签，顺序必须对齐 cron::FieldMode。
const char *kKindNames[] = {"每个", "周期", "范围", "指定具体值", "自定义"};

} // namespace

CronPanel::CronPanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    // 初始给一个最常见的真实场景（每天 9 点），一进来就能看到解读与运行时间
    applyExpr(QStringLiteral("0 9 * * *"), true);
}

void CronPanel::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    // 左右 28px 与主窗口页头对齐，视觉上像同一栏（与 CalcPanel 一致）
    layout->setContentsMargins(28, 0, 28, 20);
    layout->setSpacing(8);

    auto makeTitle = [this](const QString &text) {
        auto *lab = new QLabel(text, this);
        lab->setObjectName(QStringLiteral("cronSectionTitle"));
        // 纵向定死：QVBoxLayout 的余量会按 Preferred 策略均分给各控件，
        // 不锁高度时标题行会被拉高、整页显得松散
        lab->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        return lab;
    };

    // ---- 表达式 + 复制
    auto *exprRow = new QHBoxLayout;
    exprRow->setSpacing(8);
    m_expr = new QLineEdit(this);
    m_expr->setObjectName(QStringLiteral("cronExpr"));
    m_expr->setPlaceholderText(QStringLiteral("分 时 日 月 周（或 秒 分 时 日 月 周），支持 * , - / 与 MON~SUN"));
    m_expr->setFixedHeight(42);
    m_copy = new QPushButton(QStringLiteral("复制"), this);
    m_copy->setObjectName(QStringLiteral("cronCopyBtn"));
    exprRow->addWidget(m_expr, 1);
    exprRow->addWidget(m_copy);
    layout->addLayout(exprRow);

    // ---- 中文解读 + 状态
    m_desc = new QLabel(this);
    m_desc->setObjectName(QStringLiteral("cronDesc"));
    m_desc->setWordWrap(true);
    m_desc->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(m_desc);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("cronStatus"));
    m_status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(m_status);

    // ---- 中部双栏：左 = 可视化字段编辑，右 = 接下来 10 次运行
    auto *mainRow = new QHBoxLayout;
    mainRow->setSpacing(14);

    // == 左栏：可视化选择每个字段
    auto *leftCol = new QVBoxLayout;
    leftCol->setSpacing(8);

    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(makeTitle(QStringLiteral("可视化选择每个字段")));
    titleRow->addStretch(1);
    auto *hint = new QLabel(QStringLiteral("点选后表达式实时更新"), this);
    hint->setObjectName(QStringLiteral("cronEditorHint"));
    hint->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    titleRow->addWidget(hint);
    leftCol->addLayout(titleRow);

    // 字段 chips：互斥单选，双行显示「字段名 + 当前原文」（refresh 里更新）
    auto *chipRow = new QHBoxLayout;
    chipRow->setSpacing(6);
    m_fieldChips = new QButtonGroup(this);
    m_fieldChips->setExclusive(true);
    for (int i = 0; i < 6; ++i) {
        const auto type = static_cast<cron::FieldType>(i);
        auto *chip = new QPushButton(cron::fieldTypeName(type), this);
        chip->setObjectName(QStringLiteral("cronFieldChip"));
        chip->setCheckable(true);
        chip->setFixedSize(76, 44);
        chip->setToolTip(cron::fieldHint(type));
        m_fieldChips->addButton(chip, i);
        m_chipBtns.append(chip);
        chipRow->addWidget(chip);
    }
    chipRow->addStretch(1);
    leftCol->addLayout(chipRow);

    m_fieldTitle = new QLabel(this);
    m_fieldTitle->setObjectName(QStringLiteral("cronFieldTitle"));
    leftCol->addWidget(m_fieldTitle);

    // 五种模式单选（id 对齐 cron::FieldMode）
    auto *kindRow = new QHBoxLayout;
    kindRow->setSpacing(12);
    m_kindGroup = new QButtonGroup(this);
    m_kindGroup->setExclusive(true);
    for (int i = 0; i < 5; ++i) {
        auto *radio = new QRadioButton(QString::fromUtf8(kKindNames[i]), this);
        radio->setObjectName(QStringLiteral("cronKindRadio"));
        m_kindGroup->addButton(radio, i);
        kindRow->addWidget(radio);
    }
    kindRow->addStretch(1);
    leftCol->addLayout(kindRow);

    m_kindStack = new QStackedWidget(this);

    // [0] 每个：只有说明文案
    m_everyHint = new QLabel(this);
    m_everyHint->setWordWrap(true);
    m_kindStack->addWidget(m_everyHint);

    // [1] 周期：从第 a 起每隔 n 个单位
    auto *periodPage = new QWidget(this);
    auto *periodLay = new QHBoxLayout(periodPage);
    periodLay->setContentsMargins(0, 0, 0, 0);
    periodLay->setSpacing(6);
    periodLay->addWidget(new QLabel(QStringLiteral("从第"), periodPage));
    m_periodFrom = new QSpinBox(periodPage);
    periodLay->addWidget(m_periodFrom);
    m_periodUnitA = new QLabel(periodPage);
    periodLay->addWidget(m_periodUnitA);
    periodLay->addWidget(new QLabel(QStringLiteral("开始，每隔"), periodPage));
    m_periodStep = new QSpinBox(periodPage);
    periodLay->addWidget(m_periodStep);
    m_periodUnitB = new QLabel(periodPage);
    periodLay->addWidget(m_periodUnitB);
    periodLay->addWidget(new QLabel(QStringLiteral("执行一次"), periodPage));
    periodLay->addStretch(1);
    m_kindStack->addWidget(periodPage);

    // [2] 范围：从 a 到 b
    auto *rangePage = new QWidget(this);
    auto *rangeLay = new QHBoxLayout(rangePage);
    rangeLay->setContentsMargins(0, 0, 0, 0);
    rangeLay->setSpacing(6);
    rangeLay->addWidget(new QLabel(QStringLiteral("从"), rangePage));
    m_rangeFrom = new QSpinBox(rangePage);
    rangeLay->addWidget(m_rangeFrom);
    rangeLay->addWidget(new QLabel(QStringLiteral("到"), rangePage));
    m_rangeTo = new QSpinBox(rangePage);
    rangeLay->addWidget(m_rangeTo);
    rangeLay->addWidget(new QLabel(QStringLiteral("（范围内每个值都执行）"), rangePage));
    rangeLay->addStretch(1);
    m_kindStack->addWidget(rangePage);

    // [3] 指定具体值：可多选的值网格
    auto *valuesPage = new QWidget(this);
    auto *valuesLay = new QVBoxLayout(valuesPage);
    valuesLay->setContentsMargins(0, 0, 0, 0);
    valuesLay->setSpacing(6);
    auto *valuesTop = new QHBoxLayout;
    valuesTop->setSpacing(6);
    valuesTop->addWidget(new QLabel(QStringLiteral("点选具体的值（可多选）"), valuesPage));
    m_valuesCount = new QLabel(valuesPage);
    m_valuesCount->setObjectName(QStringLiteral("cronEditorHint"));
    valuesTop->addWidget(m_valuesCount);
    valuesTop->addStretch(1);
    auto *allBtn = new QPushButton(QStringLiteral("全选"), valuesPage);
    allBtn->setObjectName(QStringLiteral("cronMiniBtn"));
    auto *clearBtn = new QPushButton(QStringLiteral("清空"), valuesPage);
    clearBtn->setObjectName(QStringLiteral("cronMiniBtn"));
    valuesTop->addWidget(allBtn);
    valuesTop->addWidget(clearBtn);
    valuesLay->addLayout(valuesTop);

    auto *scroll = new QScrollArea(valuesPage);
    scroll->setObjectName(QStringLiteral("cronValuesScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFixedHeight(150);
    m_valuesHost = new QWidget(scroll);
    m_valuesGrid = new QGridLayout(m_valuesHost);
    m_valuesGrid->setContentsMargins(6, 6, 6, 6);
    m_valuesGrid->setHorizontalSpacing(4);
    m_valuesGrid->setVerticalSpacing(4);
    scroll->setWidget(m_valuesHost);
    valuesLay->addWidget(scroll);
    m_kindStack->addWidget(valuesPage);

    // [4] 自定义：直写字段原文
    auto *customPage = new QWidget(this);
    auto *customLay = new QVBoxLayout(customPage);
    customLay->setContentsMargins(0, 0, 0, 0);
    customLay->setSpacing(4);
    m_customEdit = new QLineEdit(customPage);
    m_customEdit->setObjectName(QStringLiteral("cronField"));
    customLay->addWidget(m_customEdit);
    auto *customHint = new QLabel(QStringLiteral("直接写这一段，例如 0-30/10、MON-FRI"), customPage);
    customHint->setObjectName(QStringLiteral("cronEditorHint"));
    customLay->addWidget(customHint);
    customLay->addStretch(1);
    m_kindStack->addWidget(customPage);

    leftCol->addWidget(m_kindStack);

    // == 右栏：接下来 10 次运行
    auto *rightCol = new QVBoxLayout;
    rightCol->setSpacing(8);
    rightCol->addWidget(makeTitle(QStringLiteral("接下来 10 次运行")));

    auto *fromRow = new QHBoxLayout;
    fromRow->setSpacing(6);
    fromRow->addWidget(new QLabel(QStringLiteral("从"), this));
    m_from = new QDateTimeEdit(QDateTime::currentDateTime(), this);
    m_from->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    m_from->setCalendarPopup(true);
    fromRow->addWidget(m_from, 1);
    m_nowBtn = new QPushButton(QStringLiteral("现在"), this);
    m_nowBtn->setObjectName(QStringLiteral("cronMiniBtn"));
    fromRow->addWidget(m_nowBtn);
    rightCol->addLayout(fromRow);

    m_next = new QListWidget(this);
    m_next->setObjectName(QStringLiteral("cronNext"));
    m_next->setSelectionMode(QAbstractItemView::NoSelection);
    m_next->setFocusPolicy(Qt::NoFocus);
    m_next->setMinimumHeight(240);
    rightCol->addWidget(m_next);

    mainRow->addLayout(leftCol, 3);
    mainRow->addLayout(rightCol, 2);
    layout->addLayout(mainRow);

    // ---- 快速预设（2 行 × 4 列）
    layout->addWidget(makeTitle(QStringLiteral("快速预设")));
    auto *presetGrid = new QGridLayout;
    presetGrid->setHorizontalSpacing(6);
    presetGrid->setVerticalSpacing(6);
    for (int i = 0; i < int(sizeof(kPresets) / sizeof(kPresets[0])); ++i) {
        auto *btn = new QPushButton(QString::fromUtf8(kPresets[i].label), this);
        btn->setObjectName(QStringLiteral("cronPresetBtn"));
        const QString expr = QString::fromUtf8(kPresets[i].expr);
        connect(btn, &QPushButton::clicked, this, [this, expr] {
            // 勾了秒字段就把预设补成 6 段（0 = 分钟起点，与标准 cron 触发时刻一致）
            applyExpr(m_withSeconds->isChecked() ? QStringLiteral("0 ") + expr : expr, true);
        });
        presetGrid->addWidget(btn, i / 4, i % 4);
    }
    layout->addLayout(presetGrid);

    m_withSeconds = new QCheckBox(QStringLiteral("含秒字段（Spring / Quartz 6 段）"), this);
    layout->addWidget(m_withSeconds);
    setSecondsVisible(false);

    // 编辑器改动 → 重建表达式（都是「写状态 → rebuildExpr」单向流，不会回环）
    connect(m_fieldChips, &QButtonGroup::idClicked, this, &CronPanel::selectField);
    connect(m_kindGroup, &QButtonGroup::idClicked, this, [this](int id) {
        const auto type = static_cast<cron::FieldType>(m_current);
        int vmin = 0, vmax = 0;
        cron::fieldRange(type, &vmin, &vmax);
        Q_UNUSED(vmax);
        const int prev = static_cast<int>(m_fields[m_current].mode);
        m_fields[m_current].mode = static_cast<cron::FieldMode>(id);
        // 切到「自定义」但框是空的：灌入上一模式拼出的原文当起点，免得一上来就报错
        if (id == static_cast<int>(cron::FieldMode::Custom)
            && m_fields[m_current].custom.trimmed().isEmpty() && prev != id) {
            cron::FieldEdit tmp = m_fields[m_current];
            tmp.mode = static_cast<cron::FieldMode>(prev);
            m_fields[m_current].custom = cron::buildFieldRaw(tmp, type, nullptr);
        }
        // 切到「指定具体值」但一个没选：预选最小值，免得立刻报「至少选一个值」
        if (id == static_cast<int>(cron::FieldMode::Values) && m_fields[m_current].values.empty())
            m_fields[m_current].values.insert(vmin);
        m_kindStack->setCurrentIndex(id);
        loadEditorFromState();
        rebuildExpr();
    });
    connect(m_periodFrom, &QSpinBox::valueChanged, this, [this](int v) {
        m_fields[m_current].periodFrom = v;
        rebuildExpr();
    });
    connect(m_periodStep, &QSpinBox::valueChanged, this, [this](int v) {
        m_fields[m_current].periodStep = v;
        rebuildExpr();
    });
    connect(m_rangeFrom, &QSpinBox::valueChanged, this, [this](int v) {
        m_fields[m_current].rangeFrom = v;
        rebuildExpr();
    });
    connect(m_rangeTo, &QSpinBox::valueChanged, this, [this](int v) {
        m_fields[m_current].rangeTo = v;
        rebuildExpr();
    });
    connect(m_customEdit, &QLineEdit::textEdited, this, [this](const QString &t) {
        m_fields[m_current].custom = t;
        rebuildExpr();
    });
    connect(allBtn, &QPushButton::clicked, this, [this] {
        for (QPushButton *b : m_valueBtns)
            b->setChecked(true);
        syncValuesFromGrid();
    });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        for (QPushButton *b : m_valueBtns)
            b->setChecked(false);
        syncValuesFromGrid();
    });

    connect(m_expr, &QLineEdit::textEdited, this, &CronPanel::onExprEdited);
    connect(m_copy, &QPushButton::clicked, this, &CronPanel::onCopy);
    connect(m_withSeconds, &QCheckBox::toggled, this, &CronPanel::onSecondsToggled);
    connect(m_from, &QDateTimeEdit::dateTimeChanged, this, [this](const QDateTime &) {
        refresh();
    });
    connect(m_nowBtn, &QPushButton::clicked, this, [this] {
        const QSignalBlocker blocker(m_from);
        m_from->setDateTime(QDateTime::currentDateTime());
        refresh();
    });

    // 默认选中「分」；秒那颗 chip 只在 6 段模式露出
    m_chipBtns[1]->setChecked(true);

    // 余量全部压到底部：内容区紧凑贴顶，窗口拉高不摊开
    layout->addStretch(1);
}

void CronPanel::applyExpr(const QString &expr, bool syncFields)
{
    // setText 不发 textEdited（只有用户输入才发），这里天然不会回环
    m_expr->setText(expr);
    if (syncFields) {
        const cron::Spec spec = cron::parse(expr);
        if (spec.valid)
            syncFieldsFrom(spec);
    }
    refresh();
}

void CronPanel::refresh()
{
    const cron::Spec spec = cron::parse(m_expr->text());
    if (!spec.valid) {
        // 解析失败不猜、不保留旧解读，避免用户把旧结论当成当前表达式的
        m_desc->setText(QStringLiteral("—"));
        setStatus(spec.error, "error");
        m_next->clear();
        return;
    }

    m_desc->setText(cron::describe(spec));
    setStatus(QStringLiteral("表达式合法"), "ok");
    updateChips(spec);

    m_next->clear();
    const QDateTime from = m_from->dateTime();
    const QVector<QDateTime> runs = cron::nextRuns(spec, from, 10);
    if (runs.isEmpty()) {
        m_next->addItem(QStringLiteral("5 年内没有匹配的运行时间（可能永不触发，如 2 月 30 日）"));
        return;
    }
    // 6 段表达式带秒精度，5 段只到分（标准 cron 在第 0 秒触发，写出来是噪音）
    const QString timeFmt = spec.hasSeconds ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm");
    for (const QDateTime &t : runs) {
        const QString when = t.toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char(' ')
                             + cron::dowName(t.date().dayOfWeek() % 7) + QLatin1Char(' ')
                             + t.toString(timeFmt);
        // 右侧相对时间跟着「从」时间走，改起算点整列一起变
        const QString rel = cron::relTimeText(from.secsTo(t));
        m_next->addItem(when + QStringLiteral("    ") + rel);
    }
}

void CronPanel::syncFieldsFrom(const cron::Spec &spec)
{
    const cron::FieldSpec specs[6] = {
        spec.second, spec.minute, spec.hour, spec.dom, spec.mon, spec.dow,
    };
    for (int i = 0; i < 6; ++i)
        m_fields[i] = cron::classifyField(specs[i], static_cast<cron::FieldType>(i));
    loadEditorFromState();
}

void CronPanel::updateChips(const cron::Spec &spec)
{
    const QString raws[6] = {
        spec.second.raw, spec.minute.raw, spec.hour.raw,
        spec.dom.raw,    spec.mon.raw,     spec.dow.raw,
    };
    for (int i = 0; i < 6; ++i) {
        const QString name = cron::fieldTypeName(static_cast<cron::FieldType>(i));
        m_chipBtns[i]->setText(name + QLatin1Char('\n') + raws[i]);
    }
}

void CronPanel::selectField(int index)
{
    if (index < 0 || index >= m_chipBtns.size())
        return;
    if (index == 0 && !m_withSeconds->isChecked())
        return; // 秒字段只在 6 段模式开放
    m_current = index;
    loadEditorFromState();
}

void CronPanel::loadEditorFromState()
{
    const auto type = static_cast<cron::FieldType>(m_current);
    int vmin = 0, vmax = 0;
    cron::fieldRange(type, &vmin, &vmax);
    const cron::FieldEdit &e = m_fields[m_current];
    const int modeId = static_cast<int>(e.mode);

    m_fieldTitle->setText(QStringLiteral("%1（%2~%3）")
                              .arg(cron::fieldTypeName(type))
                              .arg(vmin)
                              .arg(vmax));

    // idClicked 只有用户点击才发，setChecked 回填不会回环
    if (m_kindGroup->button(modeId))
        m_kindGroup->button(modeId)->setChecked(true);
    m_kindStack->setCurrentIndex(modeId);

    m_everyHint->setText(QStringLiteral("%1都触发（写出 *）").arg(cron::fieldEveryWord(type)));

    const QString unit = cron::fieldUnitWord(type);
    m_periodUnitA->setText(unit);
    m_periodUnitB->setText(unit);
    {
        const QSignalBlocker b(m_periodFrom);
        m_periodFrom->setRange(vmin, vmax);
        m_periodFrom->setValue(e.periodFrom);
    }
    {
        const QSignalBlocker b(m_periodStep);
        m_periodStep->setRange(1, vmax);
        m_periodStep->setValue(e.periodStep);
    }
    {
        const QSignalBlocker b(m_rangeFrom);
        m_rangeFrom->setRange(vmin, vmax);
        m_rangeFrom->setValue(e.rangeFrom);
    }
    {
        const QSignalBlocker b(m_rangeTo);
        m_rangeTo->setRange(vmin, vmax);
        m_rangeTo->setValue(e.rangeTo);
    }
    if (m_gridField != m_current)
        rebuildValueGrid();
    for (QPushButton *btn : m_valueBtns) {
        const QSignalBlocker b(btn);
        btn->setChecked(e.values.count(btn->property("val").toInt()) > 0);
    }
    m_valuesCount->setText(QStringLiteral("已选 %1 个").arg(int(e.values.size())));
    {
        const QSignalBlocker b(m_customEdit);
        m_customEdit->setText(e.custom);
        m_customEdit->setPlaceholderText(cron::fieldHint(type));
    }
}

void CronPanel::rebuildValueGrid()
{
    for (QPushButton *btn : m_valueBtns) {
        m_valuesGrid->removeWidget(btn);
        btn->deleteLater();
    }
    m_valueBtns.clear();

    const auto type = static_cast<cron::FieldType>(m_current);
    int vmin = 0, vmax = 0;
    cron::fieldRange(type, &vmin, &vmax);
    // 周只出 0~6：7 与 0 都是周日，出两遍会让人以为能选两个周日
    const int hi = (type == cron::FieldType::DayOfWeek) ? 6 : vmax;

    int row = 0, col = 0;
    for (int v = vmin; v <= hi; ++v) {
        auto *btn = new QPushButton(cron::valueButtonText(type, v), m_valuesHost);
        btn->setObjectName(QStringLiteral("cronValueBtn"));
        btn->setCheckable(true);
        btn->setFixedHeight(26);
        btn->setProperty("val", v);
        connect(btn, &QPushButton::clicked, this, [this] { syncValuesFromGrid(); });
        m_valuesGrid->addWidget(btn, row, col);
        m_valueBtns.append(btn);
        if (++col == 10) {
            col = 0;
            ++row;
        }
    }
    m_gridField = m_current;
}

void CronPanel::syncValuesFromGrid()
{
    std::set<int> vals;
    for (QPushButton *btn : m_valueBtns) {
        if (btn->isChecked())
            vals.insert(btn->property("val").toInt());
    }
    m_fields[m_current].values = vals;
    m_valuesCount->setText(QStringLiteral("已选 %1 个").arg(int(vals.size())));
    rebuildExpr();
}

void CronPanel::rebuildExpr()
{
    QStringList raws;
    for (int i = 0; i < 6; ++i) {
        if (i == 0 && !m_withSeconds->isChecked())
            continue; // 未勾选秒时第一段不参与拼装
        const auto type = static_cast<cron::FieldType>(i);
        QString err;
        const QString raw = cron::buildFieldRaw(m_fields[i], type, &err);
        if (raw.isEmpty()) {
            // 中间态（如自定义框正在输入 "1-"）只提示不打断，也不动顶部表达式
            setStatus(QStringLiteral("%1 字段：%2").arg(cron::fieldTypeName(type), err), "error");
            return;
        }
        raws << raw;
    }

    QString err;
    const QString expr = cron::build(raws, &err);
    if (expr.isEmpty()) {
        setStatus(err, "error");
        return;
    }
    applyExpr(expr, false); // 编辑器里就是用户当前状态，不回填
}

void CronPanel::setSecondsVisible(bool on)
{
    if (!m_chipBtns.isEmpty())
        m_chipBtns[0]->setVisible(on);
}

void CronPanel::setStatus(const QString &text, const char *state)
{
    m_status->setText(text);
    m_status->setProperty("state", QLatin1String(state));
    // 动态属性改了要 unpolish/polish 才会重算 qss（与 TranslatePanel 同）
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

void CronPanel::onExprEdited()
{
    const cron::Spec spec = cron::parse(m_expr->text());
    if (spec.valid) {
        // 表达式是 5 段还是 6 段跟着输入走：勾选与秒 chip 同步，避免两处打架。
        // setChecked 会发 toggled（→ onSecondsToggled 会改写表达式），必须挡住
        {
            const QSignalBlocker blocker(m_withSeconds);
            m_withSeconds->setChecked(spec.hasSeconds);
        }
        setSecondsVisible(spec.hasSeconds);
        if (m_current == 0 && !spec.hasSeconds)
            selectField(1); // 秒被收起就别停在秒页
        syncFieldsFrom(spec);
    }
    refresh();
}

void CronPanel::onSecondsToggled()
{
    const bool on = m_withSeconds->isChecked();
    setSecondsVisible(on);
    if (!on && m_current == 0)
        selectField(1);

    // 5 段 ↔ 6 段转换：加上秒字段（固定 0，与标准 cron 的触发时刻一致）或去掉它
    QStringList parts = m_expr->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (on && parts.size() == 5)
        parts.prepend(QStringLiteral("0"));
    else if (!on && parts.size() == 6)
        parts.removeFirst();
    applyExpr(parts.join(QLatin1Char(' ')), true);
}

void CronPanel::onCopy()
{
    QGuiApplication::clipboard()->setText(m_expr->text());
    setStatus(QStringLiteral("已复制表达式"), "ok");
}

#include "PasswordGenTool.h"

#include "LayoutMetrics.h"    // 页面级留白（与主窗口页头对齐）
#include "PasswordGenLogic.h" // 字符集 / 校验 / 生成 / 熵值（纯逻辑）
#include "ThemePalette.h"     // 颜色令牌，禁止在这里写死色值

#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QFrame>
#include <QGuiApplication>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {

/// 结果卡片：底色比页面略灰一档，靠 1px 描边收边，把密码与选项分开。
constexpr const char *kCardStyle =
    "QFrame#pwdResultCard{background:@bg-alt;border:1px solid @border;border-radius:10px;}";

QFont fixedFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

/// 小节标题（「生成选项」），统一灰蓝色小字号，与其它工具面板一致。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    return label;
}

/// 次要说明文字（熵值、底部注解）。
QLabel *makeHintLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@text-faint;"));
    return label;
}

} // namespace

///
/// 工具面板：上方结果卡片（密码 + 强度条 + 生成/复制），下方生成选项。
///
/// 交互取舍：**任何选项改动都立刻重新生成**，不设「应用」按钮 —— 用户改长度
/// 就是想看新结果，多一步点击只会让人以为没生效。代价是改长度会丢掉刚才那串，
/// 所以「复制」按钮始终在旁边，看到满意的先复制。
///
/// 所有状态都存在实例里：宿主可能同时开内嵌页与独立窗口两份面板，不能共享。
///
class PasswordPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PasswordPanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        // 左右 28px 与主窗口页头对齐；底部留 20px 避免贴边
        layout->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
        layout->setSpacing(14);

        layout->addWidget(buildResultCard());
        layout->addWidget(buildOptions());

        auto *footnote = makeHintLabel(this);
        footnote->setWordWrap(true);
        footnote->setText(QStringLiteral(
            "密码在这里生成后只存在于面板内存中：不联网、不落盘、不留历史，关掉面板就找不回来。"
            "请当场复制并保存到密码管理器里；同一个密码不要用在两个地方。"));
        layout->addWidget(footnote);
        layout->addStretch(1);   // 内容顶在上方，窗口拉高时不会散开

        // 刷新快捷键（Windows/Linux 是 F5，macOS 是 ⌘R）：与热搜等工具保持一致的手感。
        // 用 QShortcut 而不是重写 keyPressEvent，是为了让快捷键在焦点落到任何子控件时都生效。
        auto *refresh = new QShortcut(QKeySequence::Refresh, this);
        connect(refresh, &QShortcut::activated, this, &PasswordPanel::regenerate);

        regenerate();   // 打开即给一个能直接用的密码
    }

private:
    // ------------------------------------------------------------ 结果卡片

    QWidget *buildResultCard()
    {
        auto *card = new QFrame(this);
        card->setObjectName(QStringLiteral("pwdResultCard"));
        xtools::theme::applyStyle(card, QString::fromLatin1(kCardStyle));

        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->setSpacing(10);

        // 密码本体：大字号等宽字体。等宽是为了 0/O、1/l 这类容易被读错的地方
        // 至少笔形一致；只读但可选中，方便只挑其中一段。
        m_password = new QLineEdit(card);
        m_password->setReadOnly(true);
        m_password->setFont(fixedFont());
        m_password->setPlaceholderText(QStringLiteral("勾选字符集后自动生成"));
        m_password->setToolTip(QStringLiteral("密码可以直接选中，也可以点「复制」整串拿走"));
        xtools::theme::applyStyle(m_password,
            QStringLiteral("QLineEdit{background:transparent;border:none;padding:0;"
                           "font-size:22px;color:@text;}"));
        layout->addWidget(m_password);

        // 强度条 + 文字。条只负责「一眼看出强弱」，具体数字看右边熵值。
        auto *strengthRow = new QHBoxLayout;
        strengthRow->setSpacing(10);

        m_strengthBar = new QProgressBar(card);
        m_strengthBar->setRange(0, 100);
        m_strengthBar->setValue(0);
        m_strengthBar->setTextVisible(false);
        m_strengthBar->setFixedHeight(8);
        m_strengthBar->setToolTip(QStringLiteral(
            "强度按熵值估算：长度 × log2(可用字符数)。\n"
            "45 位以下只够应付临时注册，90 位以上适合重要账号。"));
        strengthRow->addWidget(m_strengthBar, 1);

        m_strengthText = new QLabel(card);
        strengthRow->addWidget(m_strengthText);

        m_entropyText = makeHintLabel(card);
        strengthRow->addWidget(m_entropyText);
        layout->addLayout(strengthRow);

        auto *buttonRow = new QHBoxLayout;
        buttonRow->setSpacing(8);

        m_regenerate = new QPushButton(QStringLiteral("重新生成"), card);
        // accent 是宿主 qss 里约定的强调按钮属性（见 style.qss 头部注释），
        // 不要用 setStyleSheet 自己拼色值。
        m_regenerate->setProperty("accent", true);
        // 提示里不能写死 "F5"：这个键在 macOS 上是 ⌘R，交给 Qt 按平台取名字
        m_regenerate->setToolTip(QStringLiteral("按 %1 也可以换一个")
                                     .arg(QKeySequence(QKeySequence::Refresh)
                                              .toString(QKeySequence::NativeText)));
        buttonRow->addWidget(m_regenerate);

        m_copy = new QPushButton(QStringLiteral("复制"), card);
        m_copy->setMinimumWidth(72);
        m_copy->setToolTip(QStringLiteral("复制整串密码到剪贴板"));
        buttonRow->addWidget(m_copy);

        buttonRow->addStretch(1);
        auto *shortcutHint = makeHintLabel(card);
        shortcutHint->setText(QStringLiteral("更改下面任一选项也会立即重新生成"));
        buttonRow->addWidget(shortcutHint);
        layout->addLayout(buttonRow);

        connect(m_regenerate, &QPushButton::clicked, this, &PasswordPanel::regenerate);
        connect(m_copy, &QPushButton::clicked, this, &PasswordPanel::copyPassword);
        return card;
    }

    // ------------------------------------------------------------ 生成选项

    QWidget *buildOptions()
    {
        auto *box = new QWidget(this);
        auto *layout = new QVBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(makeCaption(QStringLiteral("长度"), box));

        auto *lengthRow = new QHBoxLayout;
        lengthRow->setSpacing(10);
        m_length = new QSpinBox(box);
        m_length->setRange(pwdgen::kMinLength, pwdgen::kMaxLength);
        m_length->setValue(16);
        m_length->setSuffix(QStringLiteral(" 位"));
        m_length->setFixedWidth(88);
        m_length->setToolTip(QStringLiteral("16 位足够日常使用，重要账号建议 20 位以上"));
        lengthRow->addWidget(m_length);

        m_lengthSlider = new QSlider(Qt::Horizontal, box);
        m_lengthSlider->setRange(pwdgen::kMinLength, pwdgen::kMaxLength);
        m_lengthSlider->setValue(16);
        m_lengthSlider->setPageStep(4);
        lengthRow->addWidget(m_lengthSlider, 1);
        layout->addLayout(lengthRow);

        layout->addWidget(makeCaption(QStringLiteral("字符集"), box));

        auto *setRow = new QHBoxLayout;
        setRow->setSpacing(16);
        m_lower = makeSetCheck(QStringLiteral("小写 a–z"), box, setRow);
        m_upper = makeSetCheck(QStringLiteral("大写 A–Z"), box, setRow);
        m_digits = makeSetCheck(QStringLiteral("数字 0–9"), box, setRow);
        m_symbols = makeSetCheck(QStringLiteral("符号 !@#…"), box, setRow);
        setRow->addStretch(1);
        layout->addLayout(setRow);

        m_symbols->setToolTip(QStringLiteral("可选符号：%1\n"
                                             "刻意不含引号与反斜杠，避免粘进 shell / SQL / JSON 时还要转义")
                                  .arg(pwdgen::symbolChars()));

        auto *optionRow = new QHBoxLayout;
        optionRow->setSpacing(16);

        m_excludeAmbiguous = new QCheckBox(QStringLiteral("排除易混淆字符 0 O 1 l I |"), box);
        m_excludeAmbiguous->setChecked(true);
        m_excludeAmbiguous->setToolTip(QStringLiteral(
            "去掉手抄到另一台设备时最容易看错的字符。\n"
            "关掉后可用字符数会多一点、熵值略高，但输入失败的概率也更高。"));
        optionRow->addWidget(m_excludeAmbiguous);

        m_requireEach = new QCheckBox(QStringLiteral("每类字符至少出现一次"), box);
        m_requireEach->setChecked(true);
        m_requireEach->setToolTip(QStringLiteral(
            "保证勾选的每一类都至少出现一次，避免生成「只抽到小写字母」这种\n"
            "过不了网站密码规则的结果。关掉后是完全随机的均匀抽样。"));
        optionRow->addWidget(m_requireEach);
        optionRow->addStretch(1);
        layout->addLayout(optionRow);

        // 选项冲突时的提示行（例如一类字符都没勾、或长度不够放下每一类）。
        // 出错时置红并说明原因，而不是悄悄生成一个不符合设置的密码。
        m_status = new QLabel(box);
        m_status->setWordWrap(true);
        m_status->hide();
        layout->addWidget(m_status);

        // 选项的初始状态在成员初始化里定好了（见 currentOptions 的默认值），
        // 这里只负责把「改了就算」的连线补齐。
        connect(m_length, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
            // 只同步滑块；滑块自己的 valueChanged 会兜回 spin 并触发生成，
            // 这样拖动与手输都只生成一次（重复生成会让界面闪一下）。
            m_lengthSlider->setValue(value);
            regenerate();
        });
        connect(m_lengthSlider, &QSlider::valueChanged, this, [this](int value) {
            m_length->setValue(value);   // 值相同不会发信号，不会形成回环
        });
        for (QCheckBox *check : {m_lower, m_upper, m_digits, m_symbols,
                                 m_excludeAmbiguous, m_requireEach}) {
            connect(check, &QCheckBox::toggled, this, &PasswordPanel::regenerate);
        }
        return box;
    }

    /// 造一个字符集复选框并塞进行里，返回指针供后续读状态。
    QCheckBox *makeSetCheck(const QString &text, QWidget *parent, QHBoxLayout *row)
    {
        auto *check = new QCheckBox(text, parent);
        check->setChecked(true);   // 四类默认全开：四类齐全时熵值最高、也最不挑站点规则
        row->addWidget(check);
        return check;
    }

    // ------------------------------------------------------------ 行为

    /// 把界面上的选择读成逻辑层的参数。默认值与 pwdgen::Options 保持一致，
    /// 这里只覆盖用户可能改过的项。
    pwdgen::Options currentOptions() const
    {
        pwdgen::Options options;
        options.length = m_length->value();
        options.lower = m_lower->isChecked();
        options.upper = m_upper->isChecked();
        options.digits = m_digits->isChecked();
        options.symbols = m_symbols->isChecked();
        options.excludeAmbiguous = m_excludeAmbiguous->isChecked();
        options.requireEachClass = m_requireEach->isChecked();
        return options;
    }

    /// 重新生成并刷新结果区。参数不合法时清空结果、把原因写到状态行，
    /// 不保留上一次的密码 —— 屏幕上留着一串旧密码会被误以为按新设置生成过。
    void regenerate()
    {
        const pwdgen::Options options = currentOptions();

        QString error;
        const QString password = pwdgen::generate(options, &error);

        if (password.isEmpty()) {
            m_password->clear();
            m_password->setPlaceholderText(QStringLiteral("当前设置无法生成密码"));
            applyStrength(0.0);
            setStatus(error, xtools::theme::Color::Danger);
            resetCopyButton();
            return;
        }

        m_password->setText(password);
        m_password->setCursorPosition(0);   // 长密码默认显示开头，而不是被挤到末尾
        m_password->setPlaceholderText(QString());

        // 熵用「实际字符集大小」，不是按钮数量：剔掉易混淆字符后字符集会变小，
        // 这时候如果还按 26+26+10+29 算，显示的强度会虚高。
        applyStrength(pwdgen::entropyBits(options.length, pwdgen::alphabet(options).size()));
        setStatus(QString(), xtools::theme::Color::Danger);
        resetCopyButton();
    }

    /// 按熵值刷新强度条与文字。bar 的长度按 120 位熵封顶换算成百分比，
    /// 低于 3% 时也给一段可见的色块（弱密码也要让人看见它弱）。
    void applyStrength(double bits)
    {
        const pwdgen::Strength level = pwdgen::strengthOf(bits);

        QString token;
        switch (level) {
        case pwdgen::Strength::Weak:
            token = QStringLiteral("danger");
            break;
        case pwdgen::Strength::Fair:
            token = QStringLiteral("warn");
            break;
        case pwdgen::Strength::Strong:
        case pwdgen::Strength::VeryStrong:
            token = QStringLiteral("success");
            break;
        }

        // 只换 chunk 的颜色，滑块槽沿用主题的 bg-track。
        // 模板挂在控件上，宿主换肤时会按当前主题重算，不会留一块旧配色。
        xtools::theme::applyStyle(m_strengthBar,
            QStringLiteral("QProgressBar{background:@bg-track;border:none;border-radius:4px;}"
                           "QProgressBar::chunk{background:@%1;border-radius:4px;}")
                .arg(token));
        xtools::theme::applyStyle(m_strengthText,
            QStringLiteral("font-size:12px;font-weight:600;color:@%1;").arg(token));

        if (bits <= 0.0) {
            m_strengthBar->setValue(0);
            m_strengthText->clear();
            m_entropyText->clear();
            return;
        }

        const int percent = static_cast<int>(std::lround(bits * 100.0 / 120.0));
        m_strengthBar->setValue(qBound(3, percent, 100));
        m_strengthText->setText(QStringLiteral("强度：%1").arg(pwdgen::strengthLabel(level)));
        m_entropyText->setText(QStringLiteral("约 %1 位熵")
                                   .arg(QString::number(bits, 'f', 0)));
    }

    /// 错误/正常提示共用一行：有文字就显示，空字符串就隐藏。
    void setStatus(const QString &text, xtools::theme::Color token)
    {
        if (text.isEmpty()) {
            m_status->hide();
            m_status->clear();
            return;
        }
        xtools::theme::applyStyle(m_status,
            QStringLiteral("font-size:12px;color:@%1;").arg(xtools::theme::tokenName(token)));
        m_status->setText(text);
        m_status->show();
    }

    /// 复制整串密码，按钮短暂变成「已复制」再恢复。
    /// singleShot 的接收者是按钮本身：面板先被销毁时定时器随之作废，不会砸野指针。
    void copyPassword()
    {
        const QString text = m_password->text();
        if (text.isEmpty())
            return;   // 没结果时不要把剪贴板清空

        QGuiApplication::clipboard()->setText(text);
        m_copy->setText(QStringLiteral("已复制"));
        QTimer::singleShot(1200, m_copy, [this] { resetCopyButton(); });
    }

    void resetCopyButton()
    {
        m_copy->setText(QStringLiteral("复制"));
    }

    // ------------------------------------------------------------ 成员

    QLineEdit    *m_password = nullptr;
    QProgressBar *m_strengthBar = nullptr;
    QLabel       *m_strengthText = nullptr;
    QLabel       *m_entropyText = nullptr;
    QPushButton  *m_regenerate = nullptr;
    QPushButton  *m_copy = nullptr;

    QSpinBox  *m_length = nullptr;
    QSlider   *m_lengthSlider = nullptr;
    QCheckBox *m_lower = nullptr;
    QCheckBox *m_upper = nullptr;
    QCheckBox *m_digits = nullptr;
    QCheckBox *m_symbols = nullptr;
    QCheckBox *m_excludeAmbiguous = nullptr;
    QCheckBox *m_requireEach = nullptr;

    QLabel   *m_status = nullptr;
};

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo PasswordGenPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("passwordgen");
    info.name = QStringLiteral("随机密码");
    // 关键词跟着用户的叫法走：「口令」「随机字符串」「密钥」都有人这么搜，
    // 虽然严格说前两个才是同一件事，而「密钥」更多指 API secret。
    info.keywords = QStringList{
        QStringLiteral("密码"), QStringLiteral("随机密码"), QStringLiteral("密码生成"),
        QStringLiteral("生成密码"), QStringLiteral("强密码"), QStringLiteral("随机字符串"),
        QStringLiteral("口令"), QStringLiteral("password"), QStringLiteral("passwordgen"),
        QStringLiteral("generator"), QStringLiteral("secret"), QStringLiteral("token")};
    info.description = QStringLiteral("离线生成随机强密码：可选字符集与长度，实时显示熵值与强度，不保存");
    return info;
}

QWidget *PasswordGenPlugin::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存，刚生成的密码也不会在两个面板之间串
    return new PasswordPanel(parent);
}

// .cpp 里有带 Q_OBJECT 的类（PasswordPanel），需要 moc 产物
#include "PasswordGenTool.moc"

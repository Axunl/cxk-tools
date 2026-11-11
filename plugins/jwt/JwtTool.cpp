#include "JwtTool.h"

#include "JwtLogic.h"
#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用）   // 纯编解码/签名逻辑（可单独测试，见 AGENTS.md 的验证流程）

#include <QComboBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>   // Qt6 里 QPainterPath 不再由 QPainter 间接引入
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/// 状态栏 / 提示文字的颜色，集中定义避免各处写字面量漂移。
/// 语义令牌（而不是色值）：深色主题下换的是色值，调用点不用动。
namespace color {
constexpr auto error   = xtools::theme::Color::Danger;      // 解析失败、校验失败
constexpr auto success = xtools::theme::Color::Success;     // 校验通过、生成成功
constexpr auto hint    = xtools::theme::Color::TextFaint;   // 空闲提示
constexpr auto warn    = xtools::theme::Color::WarnStrong;  // alg=none 等"可用但危险"
constexpr auto ink     = xtools::theme::Color::TextBody;    // 常规信息
} // namespace color

/// 算法下拉框右侧的小三角。
///
/// 为什么不交给 qss：给 `QComboBox::drop-down` 写规则会去掉原生那条甩到圆角框外的
/// 边框，但 Qt 同时就不画默认箭头了（见 docs/PLUGIN.md 的说明）。自绘一个透明子控件
/// 最省事，位置由 JwtPanel::eventFilter 跟着下拉框尺寸摆（与 ProcMonPanel 同一套做法）。
class ComboArrow : public QWidget
{
public:
    explicit ComboArrow(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(10, 8);
        setAttribute(Qt::WA_TransparentForMouseEvents);   // 点击穿透给下拉框
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        // 自绘小三角也得跟主题走：写死浅灰在深色底上几乎看不见
        painter.setBrush(isEnabled() ? xtools::theme::color(xtools::theme::Color::TextFaint)
                                     : xtools::theme::color(xtools::theme::Color::TextDisabled));
        QPainterPath path;
        path.moveTo(0.5, 2.0);
        path.lineTo(9.5, 2.0);
        path.lineTo(5.0, 7.0);
        path.closeSubpath();
        painter.drawPath(path);
    }
};

/// 小节标题（"Header" / "Payload" 等），统一灰蓝色小字号。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;font-weight:600;color:@text-muted;"));
    return label;
}

/// 代码编辑区统一外观：等宽字体、描边圆角。
/// 颜色写 @令牌而不是字面量：插件是独立动态库，宿主换肤时只能靠这份模板重算
/// （见 api/ThemePalette.h 的 applyStyle）。
void styleCodeEditor(QPlainTextEdit *edit)
{
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    xtools::theme::applyStyle(edit, QStringLiteral(
        "QPlainTextEdit{background:@bg;border:1px solid @border;border-radius:8px;"
        "padding:6px 8px;color:@text;}"));
}

/// 状态行统一外观。
void styleStatus(QLabel *label)
{
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("font-size:12px;"));
}

void setStatus(QLabel *label, const QString &text, xtools::theme::Color token)
{
    xtools::theme::applyStyle(label, QStringLiteral("font-size:12px;color:@%1;")
                                         .arg(xtools::theme::tokenName(token)));
    label->setText(text);
}

} // namespace

///
/// 工具面板：「解码」+「编码」两页签。
///   * 解码：粘贴即解码，Header/Payload 格式化展示，iat/nbf/exp 转本地时间，
///     填密钥即时校验 HS* 签名；
///   * 编码：Header/Payload JSON + 密钥 → 实时生成令牌；算法下拉与 Header.alg 双向同步。
///
/// 状态全部存放在实例里（宿主可能同时开主窗口内嵌与独立窗口两份面板，互不影响）。
///
class JwtPanel : public QWidget
{
    Q_OBJECT

public:
    explicit JwtPanel(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        // 左右 28px 与主窗口页头对齐，视觉上像同一栏
        layout->setContentsMargins(28, 0, 28, 20);
        layout->setSpacing(10);

        m_tabs = new QTabWidget(this);
        m_tabs->setDocumentMode(true);
        layout->addWidget(m_tabs, 1);

        m_tabs->addTab(buildDecodeTab(), QStringLiteral("解码"));
        m_tabs->addTab(buildEncodeTab(), QStringLiteral("编码"));

        runDecode();   // 空输入进空闲态
        runEncode();
    }

protected:
    /// 算法下拉框的自绘箭头要跟着下拉框尺寸摆位（Resize/Show 时重摆，禁用时变淡）。
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_alg && m_algArrow) {
            switch (event->type()) {
            case QEvent::Resize:
            case QEvent::Show: {
                // 与 qss 里 ::drop-down 的 18px 箭头区对齐；窗口拉伸、字体缩放后都要重摆
                const QRect box = m_alg->rect();
                m_algArrow->move(box.width() - 18 + 4, (box.height() - m_algArrow->height()) / 2);
                break;
            }
            case QEvent::EnabledChange:
                m_algArrow->update();
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    // ------------------------------------------------------------ 页面搭建

    QWidget *buildDecodeTab()
    {
        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 12, 0, 0);
        layout->setSpacing(8);

        layout->addWidget(makeCaption(QStringLiteral("JWT 令牌"), page));
        m_tokenInput = new QPlainTextEdit(page);
        styleCodeEditor(m_tokenInput);
        m_tokenInput->setPlaceholderText(QStringLiteral("粘贴 JWT，例如 eyJhbGciOiJIUzI1NiIs..."));
        m_tokenInput->setFixedHeight(84);   // 令牌就三段，给固定三行足够
        layout->addWidget(m_tokenInput);

        auto *secretRow = new QHBoxLayout;
        secretRow->setSpacing(8);
        secretRow->addWidget(makeCaption(QStringLiteral("校验密钥（HS* 选填）"), page));
        m_secretDecode = new QLineEdit(page);
        m_secretDecode->setPlaceholderText(QStringLiteral("填入密钥后即时校验签名"));
        secretRow->addWidget(m_secretDecode, 1);
        layout->addLayout(secretRow);

        m_decodeStatus = new QLabel(page);
        styleStatus(m_decodeStatus);
        layout->addWidget(m_decodeStatus);

        auto *headerCaptionRow = new QHBoxLayout;
        headerCaptionRow->setSpacing(8);
        headerCaptionRow->addWidget(makeCaption(QStringLiteral("Header"), page));
        headerCaptionRow->addStretch(1);
        m_copyHeader = new QPushButton(QStringLiteral("复制"), page);
        headerCaptionRow->addWidget(m_copyHeader);
        layout->addLayout(headerCaptionRow);
        m_headerView = new QPlainTextEdit(page);
        styleCodeEditor(m_headerView);
        m_headerView->setReadOnly(true);
        m_headerView->setFixedHeight(96);
        layout->addWidget(m_headerView);

        auto *payloadCaptionRow = new QHBoxLayout;
        payloadCaptionRow->addWidget(makeCaption(QStringLiteral("Payload"), page));
        payloadCaptionRow->addStretch(1);
        m_copyPayload = new QPushButton(QStringLiteral("复制"), page);
        payloadCaptionRow->addWidget(m_copyPayload);
        layout->addLayout(payloadCaptionRow);
        m_payloadView = new QPlainTextEdit(page);
        styleCodeEditor(m_payloadView);
        m_payloadView->setReadOnly(true);
        layout->addWidget(m_payloadView, 1);

        m_claims = new QLabel(page);
        styleStatus(m_claims);
        layout->addWidget(m_claims);

        auto *sigRow = new QHBoxLayout;
        sigRow->setSpacing(8);
        sigRow->addWidget(makeCaption(QStringLiteral("签名"), page));
        m_sigView = new QLineEdit(page);
        m_sigView->setReadOnly(true);
        m_sigView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        sigRow->addWidget(m_sigView, 1);
        m_copySig = new QPushButton(QStringLiteral("复制"), page);
        sigRow->addWidget(m_copySig);
        layout->addLayout(sigRow);

        // 粘贴即解码、改密钥即重校验（与计算器"边输边算"同一交互）
        connect(m_tokenInput, &QPlainTextEdit::textChanged, this, &JwtPanel::runDecode);
        connect(m_secretDecode, &QLineEdit::textChanged, this, &JwtPanel::runDecode);
        connect(m_copyHeader, &QPushButton::clicked, this, [this] {
            copyToClipboard(m_headerView->toPlainText(), m_copyHeader);
        });
        connect(m_copyPayload, &QPushButton::clicked, this, [this] {
            copyToClipboard(m_payloadView->toPlainText(), m_copyPayload);
        });
        connect(m_copySig, &QPushButton::clicked, this, [this] {
            copyToClipboard(m_sigView->text(), m_copySig);
        });
        return page;
    }

    QWidget *buildEncodeTab()
    {
        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 12, 0, 0);
        layout->setSpacing(8);

        auto *topRow = new QHBoxLayout;
        topRow->setSpacing(8);
        topRow->addWidget(makeCaption(QStringLiteral("算法"), page));
        m_alg = new QComboBox(page);
        m_alg->addItems({QStringLiteral("HS256"), QStringLiteral("HS384"),
                         QStringLiteral("HS512"), QStringLiteral("none")});
        m_alg->setMinimumWidth(96);
        // 全局 qss 不画下拉三角，必须自绘（见 ComboArrow 注释）
        m_algArrow = new ComboArrow(m_alg);
        m_alg->installEventFilter(this);
        topRow->addWidget(m_alg);
        topRow->addSpacing(12);
        topRow->addWidget(makeCaption(QStringLiteral("密钥"), page));
        m_secretEncode = new QLineEdit(page);
        m_secretEncode->setPlaceholderText(QStringLiteral("HS* 必填；none 可留空"));
        topRow->addWidget(m_secretEncode, 1);
        layout->addLayout(topRow);

        layout->addWidget(makeCaption(QStringLiteral("Header（JSON）"), page));
        m_headerInput = new QPlainTextEdit(page);
        styleCodeEditor(m_headerInput);
        // 默认值就是 jwt.io 示例头部，生成结果可与主流工具对照
        m_headerInput->setPlainText(QStringLiteral("{\n    \"alg\": \"HS256\",\n    \"typ\": \"JWT\"\n}"));
        m_headerInput->setFixedHeight(96);
        layout->addWidget(m_headerInput);

        layout->addWidget(makeCaption(QStringLiteral("Payload（JSON）"), page));
        m_payloadInput = new QPlainTextEdit(page);
        styleCodeEditor(m_payloadInput);
        m_payloadInput->setPlainText(QStringLiteral(
            "{\n    \"sub\": \"1234567890\",\n    \"name\": \"John Doe\",\n    \"iat\": 1516239022\n}"));
        layout->addWidget(m_payloadInput, 1);

        auto *outCaptionRow = new QHBoxLayout;
        outCaptionRow->addWidget(makeCaption(QStringLiteral("生成的令牌"), page));
        outCaptionRow->addStretch(1);
        m_copyToken = new QPushButton(QStringLiteral("复制"), page);
        outCaptionRow->addWidget(m_copyToken);
        layout->addLayout(outCaptionRow);
        m_tokenOutput = new QPlainTextEdit(page);
        styleCodeEditor(m_tokenOutput);
        m_tokenOutput->setReadOnly(true);
        m_tokenOutput->setFixedHeight(84);
        layout->addWidget(m_tokenOutput);

        m_encodeStatus = new QLabel(page);
        styleStatus(m_encodeStatus);
        layout->addWidget(m_encodeStatus);

        // 任何输入变化都重新生成（HMAC 很便宜，无需防抖）
        connect(m_headerInput, &QPlainTextEdit::textChanged, this, [this] {
            syncAlgFromHeader();
            runEncode();
        });
        connect(m_payloadInput, &QPlainTextEdit::textChanged, this, &JwtPanel::runEncode);
        connect(m_secretEncode, &QLineEdit::textChanged, this, &JwtPanel::runEncode);
        connect(m_alg, &QComboBox::currentIndexChanged, this, &JwtPanel::applyAlgToHeader);
        connect(m_copyToken, &QPushButton::clicked, this, [this] {
            copyToClipboard(m_tokenOutput->toPlainText(), m_copyToken);
        });
        return page;
    }

    // ------------------------------------------------------------ 解码

    /// 解码当前输入并刷新展示。空输入走空闲态；解析失败只红字提示、清空旧结果，
    /// 绝不把上一次的 Header/Payload 留在屏幕上让人误读。
    void runDecode()
    {
        const QString text = m_tokenInput->toPlainText();
        if (text.trimmed().isEmpty()) {
            m_headerView->clear();
            m_payloadView->clear();
            m_sigView->clear();
            m_claims->clear();
            setStatus(m_decodeStatus,
                      QStringLiteral("粘贴 JWT 令牌，自动解码；填入密钥可校验 HS256/384/512 签名"),
                      color::hint);
            return;
        }

        const jwtlogic::DecodeResult decoded = jwtlogic::decodeToken(text);
        if (!decoded.ok) {
            m_headerView->clear();
            m_payloadView->clear();
            m_sigView->clear();
            m_claims->clear();
            setStatus(m_decodeStatus, decoded.error, color::error);
            return;
        }

        m_headerView->setPlainText(decoded.headerText);
        m_payloadView->setPlainText(decoded.payloadText);
        m_sigView->setText(decoded.signatureText.isEmpty() ? QStringLiteral("（无签名）")
                                                           : decoded.signatureText);
        updateClaims(decoded);

        const jwtlogic::VerifyResult verify =
            jwtlogic::verifyToken(decoded, m_secretDecode->text());
        xtools::theme::Color statusColor = color::ink;
        switch (verify) {
        case jwtlogic::VerifyResult::Valid:
            statusColor = color::success;
            break;
        case jwtlogic::VerifyResult::Invalid:
            statusColor = color::error;
            break;
        case jwtlogic::VerifyResult::Unsupported:
            statusColor = color::warn;
            break;
        default:
            statusColor = color::ink;
            break;
        }
        setStatus(m_decodeStatus,
                  jwtlogic::verifyResultText(verify, decoded.alg),
                  statusColor);
    }

    /// iat / nbf / exp 转本地时间；exp 过期与否用颜色区分。
    void updateClaims(const jwtlogic::DecodeResult &decoded)
    {
        if (!decoded.payloadIsObject) {
            m_claims->setText(QStringLiteral("Payload 不是 JSON 对象，已按原文展示"));
            xtools::theme::applyStyle(m_claims, QStringLiteral("font-size:12px;color:@%1;")
                                                    .arg(xtools::theme::tokenName(color::hint)));
            return;
        }

        QStringList bits;
        for (const char *key : {"iat", "nbf", "exp"}) {
            const QJsonValue value = decoded.payload.value(QLatin1String(key));
            const QString formatted = jwtlogic::formatUnixTime(value);
            if (!formatted.isEmpty())
                bits << QStringLiteral("%1 %2").arg(QLatin1String(key), formatted);
        }
        if (bits.isEmpty()) {
            m_claims->clear();
            return;
        }

        // exp 追加"已过期/有效至"结论，判断用当前时间
        const QJsonValue expValue = decoded.payload.value(QStringLiteral("exp"));
        bool expired = false;
        if (expValue.isDouble()) {
            const qint64 expSecs = static_cast<qint64>(expValue.toDouble());
            expired = QDateTime::currentSecsSinceEpoch() > expSecs;
            bits << (expired ? QStringLiteral("已过期") : QStringLiteral("有效期内"));
        }

        m_claims->setText(bits.join(QStringLiteral(" · ")));
        xtools::theme::applyStyle(m_claims, QStringLiteral("font-size:12px;color:@%1;")
                                                .arg(xtools::theme::tokenName(
                                                    expired ? color::error : color::ink)));
    }

    // ------------------------------------------------------------ 编码

    /// 重新生成令牌。失败时清空输出框（不留上一次的令牌被误复制）。
    void runEncode()
    {
        const jwtlogic::EncodeResult result =
            jwtlogic::encodeToken(m_headerInput->toPlainText(),
                                  m_payloadInput->toPlainText(),
                                  m_secretEncode->text());
        if (!result.ok) {
            m_tokenOutput->clear();
            setStatus(m_encodeStatus, result.error, color::error);
            return;
        }

        m_tokenOutput->setPlainText(result.token);
        if (result.alg == QLatin1String("none")) {
            setStatus(m_encodeStatus,
                      QStringLiteral("已生成（alg=none）：签名为空，任何人可伪造，仅限调试"),
                      color::warn);
        } else {
            setStatus(m_encodeStatus, QStringLiteral("已生成（%1）").arg(result.alg), color::success);
        }
    }

    /// 下拉框切换 → 写回 Header 的 alg 字段。
    /// setHeaderAlg() 会重排 Header 键序（见 JwtLogic.h 注释）——Header 就几个字段，
    /// 语义不变，且写回后编辑器文本就是新真源，生成结果与文本始终一致。
    void applyAlgToHeader()
    {
        if (m_updatingHeader)
            return;
        m_updatingHeader = true;
        m_headerInput->setPlainText(
            jwtlogic::setHeaderAlg(m_headerInput->toPlainText(), m_alg->currentText()));
        m_updatingHeader = false;
        // setPlainText 触发 textChanged → syncAlgFromHeader + runEncode，无需重复调用
    }

    /// 用户手改 Header.alg → 下拉框跟随（只认支持列表里的值）。
    void syncAlgFromHeader()
    {
        if (m_updatingHeader)
            return;

        QJsonParseError parseError;
        const QJsonDocument doc =
            QJsonDocument::fromJson(m_headerInput->toPlainText().toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            return;   // 文本半截/不合法时不打扰下拉框，等 runEncode 报错即可

        const QString alg = doc.object().value(QStringLiteral("alg")).toString();
        const int index = m_alg->findText(alg);
        if (index >= 0 && index != m_alg->currentIndex()) {
            m_updatingHeader = true;   // 只是同步显示，别触发 applyAlgToHeader 重写文本
            m_alg->setCurrentIndex(index);
            m_updatingHeader = false;
        }
    }

    // ------------------------------------------------------------ 通用

    /// 复制到剪贴板，按钮文案短暂变"已复制"再恢复。
    /// singleShot 带接收者（按钮），面板提前销毁时定时器随之作废，不会砸野指针。
    void copyToClipboard(const QString &text, QPushButton *button)
    {
        QGuiApplication::clipboard()->setText(text);
        button->setText(QStringLiteral("已复制"));
        QTimer::singleShot(1200, button, [button] {
            button->setText(QStringLiteral("复制"));
        });
    }

    QTabWidget    *m_tabs = nullptr;

    // 解码页
    QPlainTextEdit *m_tokenInput = nullptr;
    QLineEdit      *m_secretDecode = nullptr;
    QLabel         *m_decodeStatus = nullptr;
    QPlainTextEdit *m_headerView = nullptr;
    QPlainTextEdit *m_payloadView = nullptr;
    QLabel         *m_claims = nullptr;
    QLineEdit      *m_sigView = nullptr;
    QPushButton    *m_copyHeader = nullptr;
    QPushButton    *m_copyPayload = nullptr;
    QPushButton    *m_copySig = nullptr;

    // 编码页
    QComboBox      *m_alg = nullptr;
    ComboArrow     *m_algArrow = nullptr;
    QLineEdit      *m_secretEncode = nullptr;
    QPlainTextEdit *m_headerInput = nullptr;
    QPlainTextEdit *m_payloadInput = nullptr;
    QPlainTextEdit *m_tokenOutput = nullptr;
    QLabel         *m_encodeStatus = nullptr;
    QPushButton    *m_copyToken = nullptr;

    /// 防止"下拉框改 Header"与"Header 改下拉框"互相触发死循环。
    bool m_updatingHeader = false;
};

// ---------------------------------------------------------------- 插件入口

xtools::ToolInfo JwtPlugin::info() const
{
    xtools::ToolInfo info;
    info.id = QStringLiteral("jwt");
    info.name = QStringLiteral("JWT 加解密");
    info.keywords = QStringList{QStringLiteral("jwt"), QStringLiteral("token"),
                                QStringLiteral("解码"), QStringLiteral("编码"),
                                QStringLiteral("加解密"), QStringLiteral("签名"),
                                QStringLiteral("json web token")};
    info.description = QStringLiteral("JWT 解码、生成与 HS* 签名校验，完全离线");
    return info;
}

QWidget *JwtPlugin::createPanel(QWidget *parent)
{
    // 每次都给新面板：宿主不缓存面板，每次进入工具都会调用这里
    return new JwtPanel(parent);
}

// .cpp 里有带 Q_OBJECT 的类（JwtPanel），需要 moc 产物
#include "JwtTool.moc"

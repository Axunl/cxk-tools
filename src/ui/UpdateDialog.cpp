#include "UpdateDialog.h"

#include "Settings.h"
#include "UpdateChecker.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

namespace {

/// 主按钮文案随平台变化：Windows 是静默安装，macOS 只能打开 dmg 让用户拖。
QString primaryTextFor(const QString &kind)
{
    if (kind == QLatin1String("installer"))
        return QStringLiteral("下载并安装");
    if (kind == QLatin1String("dmg"))
        return QStringLiteral("下载安装包");
    if (kind == QLatin1String("portable"))
        return QStringLiteral("下载便携版");
    return QStringLiteral("下载");
}

QString sizeText(qint64 bytes)
{
    if (bytes <= 0)
        return QString();
    const double mb = double(bytes) / (1024.0 * 1024.0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', mb < 10.0 ? 1 : 0);
}

} // namespace

UpdateDialog::UpdateDialog(UpdateChecker *checker, Settings *settings, QWidget *parent)
    : QDialog(parent)
    , m_checker(checker)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("检查更新"));
    setMinimumWidth(560);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);

    m_title = new QLabel(this);
    m_title->setStyleSheet(QStringLiteral("color:#0F172A;font-size:17px;font-weight:600;"));
    layout->addWidget(m_title);

    m_subtitle = new QLabel(this);
    m_subtitle->setStyleSheet(QStringLiteral("color:#94A3B8;font-size:12px;"));
    layout->addWidget(m_subtitle);

    m_notes = new QTextBrowser(this);
    m_notes->setOpenExternalLinks(true);
    m_notes->setMinimumHeight(180);
    m_notes->setMaximumHeight(280);
    // 发布说明是 markdown，可能带很长的 URL 或不换行的行内代码：
    // 强制按控件宽度换行、关掉横向滚动条，否则对话框里会横着一根进度条一样的滚动条。
    m_notes->setLineWrapMode(QTextEdit::WidgetWidth);
    m_notes->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_notes, 1);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->hide();
    layout->addWidget(m_progress);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color:#94A3B8;font-size:12px;"));
    layout->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);

    m_pageButton = new QPushButton(QStringLiteral("打开发布页"), this);
    m_laterButton = new QPushButton(QStringLiteral("稍后"), this);
    m_primary = new QPushButton(QStringLiteral("下载"), this);
    m_primary->setProperty("accent", true);

    buttons->addWidget(m_pageButton);
    buttons->addWidget(m_laterButton);
    buttons->addWidget(m_primary);
    layout->addLayout(buttons);

    connect(m_primary, &QPushButton::clicked, this, &UpdateDialog::onPrimaryClicked);
    connect(m_pageButton, &QPushButton::clicked, this, &UpdateDialog::openReleasePage);
    connect(m_laterButton, &QPushButton::clicked, this, [this] {
        rememberSkip();
        close();
    });

    if (m_checker) {
        connect(m_checker, &UpdateChecker::downloadProgress, this, [this](qint64 received, qint64 total) {
            if (!m_downloading)
                return;
            if (total > 0) {
                m_progress->setRange(0, 100);
                m_progress->setValue(int(received * 100 / total));
                m_progress->setFormat(QStringLiteral("%1%").arg(int(received * 100 / total)));
            } else {
                // 没有总长度就退化成「滚动条」式的不确定进度
                m_progress->setRange(0, 0);
            }
        });

        connect(m_checker, &UpdateChecker::downloadReady, this,
                [this](const QString &path, const update::Asset &asset) {
                    m_downloading = false;
                    m_progress->hide();
                    m_progress->setRange(0, 100);
                    updateButtons();

                    // 安装版：静默跑安装器，装完由 .iss 的 [Run] 把新版本拉起来
                    if (asset.kind == QLatin1String("installer") && UpdateChecker::isInstalledBuild()) {
                        QString error;
                        if (UpdateChecker::launchInstaller(path, &error)) {
                            setStatus(QStringLiteral("正在安装，程序即将退出…"));
                            emit installerLaunched();
                            return;
                        }
                        setStatus(error, true);
                        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                        return;
                    }

                    // dmg / zip：打开它（dmg 会被挂载，zip 会交给资源管理器），剩下的步骤交给用户
                    setStatus(QStringLiteral("已下载：%1").arg(QDir::toNativeSeparators(path)));
                    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
                        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
                    }
                });

        connect(m_checker, &UpdateChecker::downloadFailed, this, [this](const QString &message) {
            m_downloading = false;
            m_progress->hide();
            m_progress->setRange(0, 100);
            updateButtons();
            setStatus(message, true);
        });
    }
}

void UpdateDialog::showUpdate(const update::Info &info)
{
    m_info = info;
    m_downloading = false;

    // 安装版下 installer，便携版下 portable：这决定了用户下一个「能直接用」的包，
    // 而不是把 setup.exe 塞给便携用户、把 zip 塞给已经安装过的用户。
    m_assetKind = UpdateChecker::isInstalledBuild() ? QStringLiteral("installer") : QStringLiteral("portable");
    m_asset = update::pickAsset(m_info, m_assetKind);
    if (!m_asset) {
        // 便携包缺失时退回任意可用包，总比不让更新好
        m_asset = update::pickAsset(m_info, QStringLiteral("installer"));
        if (m_asset)
            m_assetKind = m_asset->kind;
    }

    m_title->setText(QStringLiteral("发现新版本 %1").arg(m_info.version));
    m_subtitle->setText(QStringLiteral("当前版本 %1 · %2")
                            .arg(QApplication::applicationVersion(),
                                 m_info.notes.isEmpty() ? QStringLiteral("无更新说明") : QStringLiteral("更新说明如下")));
    m_notes->setMarkdown(m_info.notes.isEmpty()
                             ? QStringLiteral("_这个版本没有提供更新说明。_")
                             : m_info.notes);

    m_progress->hide();
    m_progress->setValue(0);

    if (!m_asset) {
        m_primary->hide();
        setStatus(QStringLiteral("当前平台没有可用的更新包，请到发布页下载"), true);
    } else {
        m_primary->show();
        m_primary->setText(primaryTextFor(m_asset->kind));
        const QString size = sizeText(m_asset->size);
        setStatus(size.isEmpty() ? QStringLiteral("下载完成后会自动校验 SHA256")
                                 : QStringLiteral("安装包 %1 · 下载完成后自动校验 SHA256").arg(size));
    }

    updateButtons();
    show();
    raise();
    activateWindow();
}

void UpdateDialog::onPrimaryClicked()
{
    if (!m_asset) {
        openReleasePage();
        return;
    }
    if (m_downloading)
        return;

    m_downloading = true;
    m_progress->show();
    m_progress->setRange(0, 0);
    setStatus(QStringLiteral("正在下载 %1…").arg(m_asset->name));
    updateButtons();

    m_checker->download(*m_asset);
}

void UpdateDialog::openReleasePage()
{
    QUrl url = m_info.pageUrl;
    if (!url.isValid())
        url = QUrl(QStringLiteral("https://github.com/Axunl/cxk-tools/releases"));
    QDesktopServices::openUrl(url);
}

void UpdateDialog::rememberSkip()
{
    if (m_settings && !m_info.version.isEmpty())
        m_settings->setSkippedVersion(m_info.version);
}

void UpdateDialog::closeEvent(QCloseEvent *event)
{
    // 下载中关窗不许「稍后」记账，否则用户下次连提示都见不到；
    // 顺带把正在进行的下载留在后台跑完（用户可能只是把窗口关了）。
    if (!m_downloading)
        rememberSkip();
    QDialog::closeEvent(event);
}

void UpdateDialog::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color:#DC2626;font-size:12px;")
                                 : QStringLiteral("color:#94A3B8;font-size:12px;"));
}

void UpdateDialog::updateButtons()
{
    const bool busy = m_downloading;
    m_primary->setEnabled(!busy);
    m_pageButton->setEnabled(!busy);
    m_laterButton->setEnabled(!busy);
    if (busy)
        m_primary->setText(QStringLiteral("下载中…"));
    else if (m_asset)
        m_primary->setText(primaryTextFor(m_asset->kind));
}

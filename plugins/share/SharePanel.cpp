#include "SharePanel.h"

#include "ThemePalette.h"   // 颜色令牌（宿主与插件共用）

#include "ShareServer.h"
#include "ShareStore.h"

#include <QAction>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

// 共享模型与服务的类型都在 share 命名空间里；这个 .cpp 里引用它们的次数很多，
// 逐个写 share:: 前缀只会让调用点更难读，这里统一 using。
using namespace share;

namespace {

/// 卡片外壳。QFrame 子类必须显式开 WA_StyledBackground 才绘制 qss 的圆角背景。
QFrame *makeCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("fsCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    return card;
}

/// 小灰字（说明 / 统计）。
QLabel *makeCaption(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("fsCaption"));
    return label;
}

/// 卡片标题（比正文重一点，比页头轻一点）。
QLabel *makeCardTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("fsCardTitle"));
    return label;
}

/// 卡片工具栏里的小按钮。全局 QPushButton 的 padding 是 6px 14px，
/// 一排四五个按钮会把它撑得很高、把列表挤没，所以这里显式缩小（见 style.qss）。
QPushButton *makeMiniButton(const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(QStringLiteral("fsMiniBtn"));
    return button;
}

/// 动态属性变了要 unpolish/polish 才会重新匹配 qss 里的 [state="..."] 规则。
void setWidgetState(QWidget *widget, const char *state)
{
    if (!widget)
        return;
    const QString wanted = QString::fromLatin1(state);
    if (widget->property("state").toString() == wanted)
        return;
    widget->setProperty("state", wanted);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

/// 单行文本预览：列表项只有一行高，多行内容压成空格分隔，超长截断。
QString previewOf(const QString &text)
{
    QString flat = text;
    flat.replace(QLatin1Char('\r'), QLatin1Char(' '));
    flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
    flat = flat.simplified();
    if (flat.size() > 46)
        flat = flat.left(46) + QStringLiteral("…");
    return flat;
}

} // namespace

// ------------------------------------------------------------------ 构造

SharePanel::SharePanel(QWidget *parent)
    : QWidget(parent)
{
    // 拖文件进来直接共享：共享工具最自然的手势，比"点添加文件"快得多
    setAcceptDrops(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 0, 28, 20);   // 与宿主页头 28px 留白对齐
    root->setSpacing(10);

    root->addWidget(buildAddressCard());

    auto *hint = new QLabel(
        QStringLiteral("同一局域网内的手机 / 电脑用浏览器打开上面的地址，就能上传下载文件、收发文本。"
                       "关掉这个工具不会中断共享，正在下载的浏览器也不会被打断。"),
        this);
    hint->setObjectName(QStringLiteral("fsHint"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);
    columns->addWidget(buildFileCard(), 1);
    columns->addWidget(buildTextCard(), 1);
    root->addLayout(columns, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("fsStatus"));
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    // 网卡地址会随换网/插拔网线变化，面板可见时每 5 秒对一次；隐藏就停（别白耗）。
    m_addressTimer = new QTimer(this);
    m_addressTimer->setInterval(5000);
    connect(m_addressTimer, &QTimer::timeout, this, [this] { refreshAddress(false); });

    connect(&ShareStore::instance(), &ShareStore::changed, this, [this] {
        refreshFiles();
        refreshTexts();
    });
    connect(&ShareServer::instance(), &ShareServer::statusChanged, this, [this] {
        refreshAddress(true);
    });

    startServer();
    refreshFiles();
    refreshTexts();
}

// ------------------------------------------------------------------ 地址区

QWidget *SharePanel::buildAddressCard()
{
    QFrame *card = makeCard(this);
    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);

    m_dot = new QLabel(card);
    m_dot->setObjectName(QStringLiteral("fsDot"));
    m_dot->setFixedSize(8, 8);
    layout->addWidget(m_dot);

    layout->addWidget(makeCaption(QStringLiteral("共享地址"), card));

    m_urlEdit = new QLineEdit(card);
    m_urlEdit->setObjectName(QStringLiteral("fsUrl"));
    m_urlEdit->setReadOnly(true);
    m_urlEdit->setPlaceholderText(QStringLiteral("共享服务未启动"));
    m_urlEdit->setMinimumWidth(200);
    layout->addWidget(m_urlEdit, 1);

    m_copyBtn = new QPushButton(QStringLiteral("复制链接"), card);
    m_copyBtn->setProperty("accent", true);
    layout->addWidget(m_copyBtn);

    m_openBtn = new QPushButton(QStringLiteral("打开网页"), card);
    layout->addWidget(m_openBtn);

    // 多网卡（有线 + WiFi + VPN + 虚拟网卡）时让人挑一个再复制，
    // 默认给真正在用的那个（排序见 ShareHttp.h 的 localAddresses）。
    // 用文字按钮而不是 ▾ 三角：U+25BE 在部分中文字体里是豆腐块（实测截图就是方块）。
    m_switchBtn = new QPushButton(QStringLiteral("切换"), card);
    m_switchBtn->setObjectName(QStringLiteral("fsSwitch"));
    m_switchBtn->setToolTip(QStringLiteral("切换要发布的网卡地址"));
    m_switchBtn->setVisible(false);
    layout->addWidget(m_switchBtn);

    layout->addSpacing(4);
    m_badge = new QLabel(card);
    m_badge->setObjectName(QStringLiteral("fsBadge"));
    layout->addWidget(m_badge);

    connect(m_copyBtn, &QPushButton::clicked, this, &SharePanel::copyLink);
    connect(m_openBtn, &QPushButton::clicked, this, &SharePanel::openInBrowser);

    return card;
}

// ------------------------------------------------------------------ 文件区

QWidget *SharePanel::buildFileCard()
{
    QFrame *card = makeCard(this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto *head = new QHBoxLayout;
    head->setSpacing(6);
    head->addWidget(makeCardTitle(QStringLiteral("共享文件"), card));
    head->addSpacing(4);

    auto *addFileBtn = makeMiniButton(QStringLiteral("添加文件"), card);
    auto *addDirBtn = makeMiniButton(QStringLiteral("添加文件夹"), card);
    head->addWidget(addFileBtn);
    head->addWidget(addDirBtn);
    head->addStretch(1);

    m_fileRemoveBtn = makeMiniButton(QStringLiteral("移除"), card);
    m_fileClearBtn = makeMiniButton(QStringLiteral("清空"), card);
    head->addWidget(m_fileRemoveBtn);
    head->addWidget(m_fileClearBtn);
    layout->addLayout(head);

    m_fileTree = new QTreeWidget(card);
    m_fileTree->setObjectName(QStringLiteral("fsFileList"));
    m_fileTree->setColumnCount(2);
    m_fileTree->setHeaderLabels({QStringLiteral("文件"), QStringLiteral("大小")});
    m_fileTree->setRootIsDecorated(false);
    m_fileTree->setUniformRowHeights(true);
    m_fileTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_fileTree->setColumnWidth(1, 78);
    // 数值列右对齐，表头也跟着右对齐，否则"大小"两个字会贴在左边界、与数字错开
    m_fileTree->headerItem()->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_fileTree, 1);

    m_fileCount = makeCaption(QStringLiteral("共 0 个文件"), card);
    layout->addWidget(m_fileCount);

    connect(addFileBtn, &QPushButton::clicked, this, &SharePanel::addFilesDialog);
    connect(addDirBtn, &QPushButton::clicked, this, &SharePanel::addFolderDialog);
    connect(m_fileRemoveBtn, &QPushButton::clicked, this, &SharePanel::removeSelectedFile);
    connect(m_fileClearBtn, &QPushButton::clicked, this, &SharePanel::clearFiles);
    connect(m_fileTree, &QTreeWidget::itemSelectionChanged, this, [this] {
        m_fileRemoveBtn->setEnabled(!m_fileTree->selectedItems().isEmpty());
    });
    connect(m_fileTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        // 双击 = 在本机浏览器里打开这个文件的下载地址，方便自测服务是否真的通
        if (!item)
            return;
        const QString url = currentUrl();
        const QString id = item->data(0, Qt::UserRole).toString();
        if (!url.isEmpty() && !id.isEmpty())
            QDesktopServices::openUrl(QUrl(url + QStringLiteral("/f/") + id));
    });

    return card;
}

// ------------------------------------------------------------------ 文本区

QWidget *SharePanel::buildTextCard()
{
    QFrame *card = makeCard(this);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto *head = new QHBoxLayout;
    head->setSpacing(6);
    head->addWidget(makeCardTitle(QStringLiteral("共享文本"), card));
    head->addStretch(1);
    m_textRemoveBtn = makeMiniButton(QStringLiteral("删除"), card);
    m_textClearBtn = makeMiniButton(QStringLiteral("清空"), card);
    head->addWidget(m_textRemoveBtn);
    head->addWidget(m_textClearBtn);
    layout->addLayout(head);

    m_textList = new QListWidget(card);
    m_textList->setObjectName(QStringLiteral("fsTextList"));
    m_textList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_textList->setWordWrap(false);
    m_textList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_textList, 1);

    // 桌面端只发单行：日常共享的是路径、命令、链接这类一句话内容；
    // 需要发多行大段文本时用网页端（那边是 textarea，支持 Ctrl+Enter 发送）。
    auto *sendRow = new QHBoxLayout;
    sendRow->setSpacing(8);
    m_textInput = new QLineEdit(card);
    m_textInput->setObjectName(QStringLiteral("fsTextInput"));
    m_textInput->setPlaceholderText(QStringLiteral("输入文本，回车发送给所有打开网页的设备"));
    sendRow->addWidget(m_textInput, 1);
    auto *sendBtn = new QPushButton(QStringLiteral("发送"), card);
    sendBtn->setProperty("accent", true);
    sendRow->addWidget(sendBtn);
    layout->addLayout(sendRow);

    m_textCount = makeCaption(QStringLiteral("共 0 条"), card);
    layout->addWidget(m_textCount);

    connect(sendBtn, &QPushButton::clicked, this, &SharePanel::sendText);
    connect(m_textInput, &QLineEdit::returnPressed, this, &SharePanel::sendText);
    connect(m_textRemoveBtn, &QPushButton::clicked, this, &SharePanel::removeSelectedText);
    connect(m_textClearBtn, &QPushButton::clicked, this, [this] {
        ShareStore::instance().clearTexts();
        setStatus(QStringLiteral("已清空共享文本"));
    });
    connect(m_textList, &QListWidget::itemSelectionChanged, this, [this] {
        m_textRemoveBtn->setEnabled(!m_textList->selectedItems().isEmpty());
    });
    connect(m_textList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        const TextEntry *entry =
            ShareStore::instance().findText(item->data(Qt::UserRole).toString());
        if (!entry)
            return;
        QGuiApplication::clipboard()->setText(entry->text);
        setStatus(QStringLiteral("已复制这条文本到剪贴板"));
    });

    return card;
}

// ------------------------------------------------------------------ 服务

void SharePanel::startServer()
{
    ShareServer &server = ShareServer::instance();
    QString error;
    if (!server.start(ShareServer::kDefaultPort, &error)) {
        // 规则 3 的同类要求：做不到就说清原因，不要留一个"看着在共享其实没监听"的界面
        m_urlEdit->clear();
        m_urlEdit->setPlaceholderText(QStringLiteral("共享服务未启动"));
        m_badge->setText(QStringLiteral("未启动"));
        m_copyBtn->setEnabled(false);
        m_openBtn->setEnabled(false);
        setWidgetState(m_dot, "off");
        setWidgetState(m_badge, "off");
        setStatus(error, true);
        return;
    }

    refreshAddress(true);
    setStatus(QStringLiteral("已发布共享地址，同一网段的设备用浏览器打开即可。"));
}

void SharePanel::refreshAddress(bool force)
{
    ShareServer &server = ShareServer::instance();
    m_addresses = server.addresses();

    QStringList urls;
    for (const share::LocalAddress &address : m_addresses)
        urls << address.url;

    // 地址没变就不动界面：每 5 秒重设一遍 QLineEdit 会打断正在进行的选中/复制
    if (!force && urls == m_knownUrls)
        return;
    m_knownUrls = urls;

    // 用户手动选过的地址若还在，就继续用它；否则回到首选（家用局域网优先）
    bool stillValid = false;
    for (const share::LocalAddress &address : m_addresses) {
        if (address.url == m_selectedUrl)
            stillValid = true;
    }
    if (!stillValid)
        m_selectedUrl = server.primaryUrl();

    const bool running = server.isRunning() && !m_selectedUrl.isEmpty();
    m_urlEdit->setText(m_selectedUrl);
    m_copyBtn->setEnabled(running);
    m_openBtn->setEnabled(running);
    m_badge->setText(running ? QStringLiteral("正在共享") : QStringLiteral("未启动"));
    setWidgetState(m_badge, running ? "on" : "off");
    setWidgetState(m_dot, running ? "on" : "off");
    m_switchBtn->setVisible(m_addresses.size() > 1);
    rebuildAddressMenu();
}

void SharePanel::rebuildAddressMenu()
{
    if (m_switchBtn->menu())
        m_switchBtn->menu()->deleteLater();   // setMenu 不接管旧菜单，不删会泄漏

    auto *menu = new QMenu(m_switchBtn);
    for (const share::LocalAddress &address : m_addresses) {
        QAction *action = menu->addAction(address.label);
        action->setCheckable(true);
        action->setChecked(address.url == m_selectedUrl);
        const QString url = address.url;
        connect(action, &QAction::triggered, this, [this, url] {
            m_selectedUrl = url;
            m_urlEdit->setText(url);
            rebuildAddressMenu();
            setStatus(QStringLiteral("已切换到 %1").arg(url));
        });
    }
    if (menu->isEmpty()) {
        QAction *empty = menu->addAction(QStringLiteral("没有可用的局域网地址"));
        empty->setEnabled(false);
    }
    m_switchBtn->setMenu(menu);
}

// ------------------------------------------------------------------ 列表

void SharePanel::refreshFiles()
{
    const ShareStore &store = ShareStore::instance();

    // 重建前记住选中项（否则每加一个文件，之前选中的行就被清掉了）
    QSet<QString> keepSelected;
    const auto selectedItems = m_fileTree->selectedItems();
    for (const QTreeWidgetItem *item : selectedItems)
        keepSelected.insert(item->data(0, Qt::UserRole).toString());

    m_fileTree->clear();
    qint64 totalBytes = 0;
    const QList<FileEntry> &files = store.files();
    for (const FileEntry &entry : files) {
        auto *item = new QTreeWidgetItem(m_fileTree);
        item->setData(0, Qt::UserRole, entry.id);
        item->setText(1, humanSize(entry.size));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(0, entry.path);

        if (QFileInfo::exists(entry.path)) {
            item->setText(0, entry.name);
            totalBytes += entry.size;
        } else {
            // 源文件被移走/删掉：列表里留着但明确标出来，否则用户会以为服务坏了
            item->setText(0, entry.name + QStringLiteral("（已丢失）"));
            // 丢失的文件用"禁用文字"色：深色主题下也要能看清是灰掉而不是无色
            const QBrush missing(xtools::theme::color(xtools::theme::Color::TextDisabled));
            item->setForeground(0, missing);
            item->setForeground(1, missing);
            item->setToolTip(0, entry.path + QStringLiteral("\n文件已不在这个路径上"));
        }
        if (keepSelected.contains(entry.id))
            item->setSelected(true);
    }

    m_fileCount->setText(files.isEmpty()
                             ? QStringLiteral("还没有共享文件：点「添加文件」，或把文件拖进来")
                             : QStringLiteral("共 %1 个文件 · 合计 %2")
                                   .arg(files.size())
                                   .arg(humanSize(totalBytes)));
    m_fileRemoveBtn->setEnabled(!m_fileTree->selectedItems().isEmpty());
    m_fileClearBtn->setEnabled(!files.isEmpty());
}

void SharePanel::refreshTexts()
{
    const ShareStore &store = ShareStore::instance();

    QSet<QString> keepSelected;
    const auto selectedItems = m_textList->selectedItems();
    for (const QListWidgetItem *item : selectedItems)
        keepSelected.insert(item->data(Qt::UserRole).toString());

    m_textList->clear();
    const QList<TextEntry> &texts = store.texts();
    for (const TextEntry &entry : texts) {
        auto *item = new QListWidgetItem(m_textList);
        item->setData(Qt::UserRole, entry.id);
        item->setText(QStringLiteral("%1  ·  %2").arg(previewOf(entry.text), entry.from));
        item->setToolTip(QStringLiteral("%1\n\n来自 %2 · %3")
                             .arg(entry.text, entry.from,
                                  entry.createdAt.toString(QStringLiteral("MM-dd HH:mm"))));
        if (keepSelected.contains(entry.id))
            item->setSelected(true);
    }

    m_textCount->setText(texts.isEmpty()
                             ? QStringLiteral("还没有共享文本：在下面输入后回车")
                             : QStringLiteral("共 %1 条 · 双击复制").arg(texts.size()));
    m_textRemoveBtn->setEnabled(!m_textList->selectedItems().isEmpty());
    m_textClearBtn->setEnabled(!texts.isEmpty());
}

// ------------------------------------------------------------------ 添加 / 删除

void SharePanel::addFilesDialog()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择要共享的文件"), QString(),
        QStringLiteral("所有文件 (*.*)"));
    if (!paths.isEmpty())
        addLocalPaths(paths);
}

void SharePanel::addFolderDialog()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择要共享的文件夹"));
    if (dir.isEmpty())
        return;
    addLocalPaths({dir});
}

void SharePanel::addLocalPaths(const QStringList &paths)
{
    QStringList problems;
    int added = 0;

    for (const QString &path : paths) {
        const QFileInfo info(path);
        QString error;
        if (info.isDir()) {
            added += ShareStore::instance().addDirectory(path, &error);
        } else if (ShareStore::instance().addFile(path, &error)) {
            ++added;
        }
        // 去重未命中、超上限、目录为空……都汇总到最后一行说，避免弹出四个框
        if (!error.isEmpty())
            problems << QStringLiteral("%1：%2").arg(info.fileName(), error);
    }

    if (added > 0 && problems.isEmpty()) {
        setStatus(QStringLiteral("已加入 %1 个文件").arg(added));
    } else if (added > 0) {
        setStatus(QStringLiteral("已加入 %1 个文件；%2").arg(added).arg(problems.join(QStringLiteral("；"))),
                  true);
    } else if (!problems.isEmpty()) {
        setStatus(problems.join(QStringLiteral("；")), true);
    }
}

void SharePanel::removeSelectedFile()
{
    // 先把 id 拷出来再删：删除会触发 changed → refreshFiles() 清空树，
    // 直接遍历 selectedItems() 会在第二次迭代踩到已销毁的 item。
    QStringList ids;
    const auto items = m_fileTree->selectedItems();
    for (const QTreeWidgetItem *item : items)
        ids << item->data(0, Qt::UserRole).toString();
    if (ids.isEmpty())
        return;

    int removed = 0;
    for (const QString &id : ids) {
        if (ShareStore::instance().removeFile(id))
            ++removed;
    }
    setStatus(QStringLiteral("已移除 %1 个文件").arg(removed));
}

void SharePanel::removeSelectedText()
{
    QStringList ids;
    const auto items = m_textList->selectedItems();
    for (const QListWidgetItem *item : items)
        ids << item->data(Qt::UserRole).toString();
    if (ids.isEmpty())
        return;

    int removed = 0;
    for (const QString &id : ids) {
        if (ShareStore::instance().removeText(id))
            ++removed;
    }
    setStatus(QStringLiteral("已删除 %1 条文本").arg(removed));
}

void SharePanel::clearFiles()
{
    const int count = ShareStore::instance().files().size();
    if (count == 0)
        return;

    // 网页上传的副本会被一起删掉，所以必须二次确认；用户自己的原文件不受影响。
    const auto answer = QMessageBox::question(
        this, QStringLiteral("清空共享文件"),
        QStringLiteral("从共享列表移除全部 %1 个文件？\n\n"
                       "网页上传的副本会连磁盘文件一起删除；"
                       "你自己硬盘上的原文件不受影响。")
            .arg(count),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    ShareStore::instance().clearFiles();
    setStatus(QStringLiteral("已清空共享文件"));
}

// ------------------------------------------------------------------ 动作

void SharePanel::sendText()
{
    const QString text = m_textInput->text();
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral("先输入点内容再发送"), true);
        return;
    }

    QString error;
    if (ShareStore::instance().addText(text, QStringLiteral("本机"), &error).isEmpty()) {
        setStatus(error, true);
        return;
    }
    m_textInput->clear();
    setStatus(QStringLiteral("已发送，网页端最迟 2 秒后就能看到"));
}

void SharePanel::copyLink()
{
    const QString url = currentUrl();
    if (url.isEmpty()) {
        setStatus(QStringLiteral("共享服务未启动，没有可复制的地址"), true);
        return;
    }

    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        setStatus(QStringLiteral("剪贴板不可用"), true);
        return;
    }
    clipboard->setText(url);
    // setText() 没有返回值：Windows 上被别的程序占住时只会往 stderr 打一条警告。
    // 不回读校验就会把失败当成功报给用户（取色器/文本处理踩过同一个坑）。
    if (clipboard->text() != url) {
        setStatus(QStringLiteral("复制失败（剪贴板可能被其他程序占用），请手动复制"), true);
        return;
    }
    setStatus(QStringLiteral("已复制共享地址：%1").arg(url));
}

void SharePanel::openInBrowser()
{
    const QString url = currentUrl();
    if (url.isEmpty()) {
        setStatus(QStringLiteral("共享服务未启动，无法打开网页"), true);
        return;
    }
    QDesktopServices::openUrl(QUrl(url));
}

QString SharePanel::currentUrl() const
{
    if (!ShareServer::instance().isRunning())
        return QString();
    return m_selectedUrl;
}

void SharePanel::setStatus(const QString &text, bool error)
{
    if (!m_status)
        return;
    m_status->setText(text);
    setWidgetState(m_status, error ? "error" : "info");
}

// ------------------------------------------------------------------ 事件

void SharePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_addressTimer->start();
    // 面板可能刚从独立窗口那边带过来（两边共用同一份数据），进来先对一次
    refreshAddress(false);
    refreshFiles();
    refreshTexts();
}

void SharePanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_addressTimer->stop();
}

void SharePanel::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData() && event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void SharePanel::dragLeaveEvent(QDragLeaveEvent *event)
{
    QWidget::dragLeaveEvent(event);
}

void SharePanel::dropEvent(QDropEvent *event)
{
    if (!event->mimeData() || !event->mimeData()->hasUrls())
        return;

    QStringList paths;
    const auto urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    event->acceptProposedAction();
    if (!paths.isEmpty())
        addLocalPaths(paths);
}

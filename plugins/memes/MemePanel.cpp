#include "MemePanel.h"
#include "LayoutMetrics.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace meme {

namespace {

/// 搜索防抖。500ms 是折中：中文输入法组字阶段 textChanged 会连续触发，
/// 太短会拿半成品去搜；太长又会让人觉得"打完还得等一下"。
constexpr int kDebounceMs = 500;
/// 搜索页很小，8 秒足够；图片大一些，12 秒。
constexpr int kSearchTimeoutMs = 8000;
constexpr int kImageTimeoutMs = 12000;
/// 发表情失败后的冷却时长。实测站点偶发长时间挂起（单次请求 25 秒都不回），
/// 冷却期内跳过主源，直接给 360 的结果，用户不用每次都先等满 8 秒。
constexpr int kFabCooldownMs = 5 * 60 * 1000;
/// 单张图硬上限。防的是"图床返回一张几十兆的图"或者压缩炸弹，不是正常表情包。
constexpr int kMaxImageBytes = 8 * 1024 * 1024;
/// 缓存容量：先到先限。
constexpr int kCacheMaxFiles = 300;
constexpr qint64 kCacheMaxBytes = 120LL * 1024 * 1024;
/// 每成功写入这么多张就跑一次清理，避免每次落盘都扫目录。
constexpr int kPruneEveryStores = 50;
/// 面板低于这个高度就把「常用词」那行收起来，把空间让给网格。
constexpr int kChipsMinHeight = 480;
/// 滚动时用 60ms 合并连续滚动事件，避免一次拖拽触发上百次调度。
constexpr int kPlaybackSettleMs = 60;

/// 常用词。挑的都是覆盖面广、结果质量稳定的口语词，不放时效性强的网络热词。
const char *const kChips[] = {"无语", "笑哭", "点赞", "震惊", "摸鱼", "加油"};

QString userAgent()
{
    // 版本号可能为空（例如被测试程序直接链接、或没走宿主 main.cpp），
    // 这时不能发一个 "cxk-tools/" 这样带尾巴的 UA —— 服务端会当成垃圾请求挡掉。
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? QStringLiteral("cxk-tools")
                             : QStringLiteral("cxk-tools/%1").arg(version);
}

/// 统一构造请求。Referer 只有发表情的图床校验，传空就不加。
QNetworkRequest makeRequest(const QUrl &url, const QString &referer, int timeoutMs)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Accept-Language", "zh-CN,zh;q=0.9");
    if (!referer.isEmpty())
        request.setRawHeader("Referer", referer.toUtf8());
    return request;
}

/// 把网络错误翻译成人能看懂的失败原因。
/// 403 单独说：发表情的图床在没有 Referer 或防盗链策略变了时会回 403，
/// 这种时候光显示 "Forbidden" 帮不上忙。
/// 超时也要单独说：Qt 的传输超时是以 OperationCanceledError 上报的，
/// 直接把 errorString() 抛给用户会得到一句莫名其妙的 "Operation canceled"。
QString describeReplyError(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403)
        return QStringLiteral("图片被拒绝访问（403，可能是防盗链策略变化）");
    if (status == 404)
        return QStringLiteral("图片已失效（404）");
    if (status > 0 && status != 200)
        return QStringLiteral("服务器返回 HTTP %1").arg(status);

    switch (reply->error()) {
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("请求超时或已中止");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("域名解析失败，检查网络");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("连接被拒绝");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("服务器中断了连接");
    default:
        break;
    }
    return reply->errorString();
}

} // namespace

QMimeData *buildClipboardMime(const QByteArray &bytes, const QImage &firstFrame,
                              const QString &filePath, bool animated)
{
    auto *mime = new QMimeData;

#if defined(Q_OS_WIN)
    if (animated) {
        // Qt 文档约定：application/x-qt-windows-mime;value="X" 会注册成同名 Windows
        // 剪贴板格式。直接 setData("image/gif") 会被 QWindowsMimeImage 当成普通图像
        // 转成 CF_DIB（单帧），原始 GIF 字节就丢了。
        mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"GIF\""), bytes);
    }
#elif defined(Q_OS_MACOS)
    // macOS 刻意不设 image/gif：Qt 没有这张 UTI 映射，设了只会变成
    // com.trolltech.anymime.image--gif，原生应用读不到，反而误导。
    Q_UNUSED(bytes);
    Q_UNUSED(animated);
#else
    if (animated)
        mime->setData(QStringLiteral("image/gif"), bytes);   // X11 按 MIME 原样导出
    else
        Q_UNUSED(bytes);
#endif

    // 静态兜底。QMimeData 没有 setImage()，图像走的是 setImageData()：
    // 它把 QImage 放进 application/x-qt-image，贴到只认位图的应用（微信/QQ 输入框
    // 读 CF_DIB、macOS 读 public.tiff）时就是这一份。
    mime->setImageData(firstFrame);

    // 文件通道：Windows 走 CF_HDROP、macOS 走 public.file-url、X11 走 text/uri-list。
    // 指向 outbox 里的稳定副本，缓存被清理也不会让这个路径失效。
    mime->setUrls({QUrl::fromLocalFile(filePath)});
    return mime;
}

MemePanel::MemePanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] {
        submitSearch(m_search->text());
    });

    m_playbackTimer.setSingleShot(true);
    m_playbackTimer.setInterval(kPlaybackSettleMs);
    connect(&m_playbackTimer, &QTimer::timeout, this, [this] { refreshPlayback(); });

    // 构造后延迟清理一次缓存：不阻塞面板首次显示。
    QTimer::singleShot(1500, this, [this] {
        m_cache.prune(kCacheMaxFiles, kCacheMaxBytes);
    });

    // 运行环境没有 GIF 解码插件时必须说出来，否则用户只会看到"角标是 GIF 但图不动"。
    if (!QImageReader::supportedImageFormats().contains("gif")) {
        setStatus(QStringLiteral("当前环境缺少 GIF 解码插件，动图将只显示首帧"), 
                  xtools::theme::Color::Warn);
    } else {
        restoreBaseStatus();
    }
}

// ------------------------------------------------------------------ 界面

void MemePanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    // 左右 28px 与宿主页头对齐（所有插件面板的统一约定）；别在这里写标题，
    // 页头由宿主提供，重复写会出现两个标题。
    root->setContentsMargins(xtools::layout::kPageMargin, 0, xtools::layout::kPageMargin, 20);
    root->setSpacing(10);

    // ---- 搜索行
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(10);

    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("memeSearch"));
    m_search->setPlaceholderText(QStringLiteral("输入关键词搜表情包，例如：无语 / 摸鱼 / 点赞"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(36);
    searchRow->addWidget(m_search, 1);

    m_searchButton = new QPushButton(QStringLiteral("搜索"), this);
    m_searchButton->setProperty("accent", true);
    m_searchButton->setCursor(Qt::PointingHandCursor);
    m_searchButton->setFixedHeight(36);
    searchRow->addWidget(m_searchButton, 0);
    root->addLayout(searchRow);

    // ---- 常用词
    m_chipRow = new QWidget(this);
    auto *chipLayout = new QHBoxLayout(m_chipRow);
    chipLayout->setContentsMargins(0, 0, 0, 0);
    chipLayout->setSpacing(8);
    for (const char *word : kChips) {
        const QString label = QString::fromUtf8(word);
        auto *chip = new QPushButton(label, m_chipRow);
        chip->setObjectName(QStringLiteral("memeChip"));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFlat(true);
        connect(chip, &QPushButton::clicked, this, [this, label] {
            m_search->setText(label);
            submitSearch(label);
        });
        chipLayout->addWidget(chip);
    }
    chipLayout->addStretch(1);
    applyChipStyle();
    root->addWidget(m_chipRow);

    // ---- 网格
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    // 横向滚动一律关掉：瓦片按可用宽度换列，不需要左右滚。
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    xtools::theme::applyStyle(m_scroll,
                              QStringLiteral("QScrollArea{background:transparent;border:none;}"));
    m_grid = new MemeGrid(m_scroll);
    m_grid->onLoadMore = [this] {
        if (m_loading || m_noMore)
            return;
        requestPage(m_page + 1);
    };
    m_scroll->setWidget(m_grid);
    root->addWidget(m_scroll, 1);

    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        m_playbackTimer.start();
    });

    // ---- 状态行
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(10);
    m_status = new QLabel(this);
    xtools::theme::applyStyle(m_status, QStringLiteral("font-size:12px;color:@text-faint;"));
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    statusRow->addWidget(m_status, 1);
    m_count = new QLabel(this);
    xtools::theme::applyStyle(m_count, QStringLiteral("font-size:12px;color:@text-faint;"));
    m_count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusRow->addWidget(m_count, 0);
    root->addLayout(statusRow);

    // ---- 预览浮层（子控件，铺满面板，默认隐藏）
    m_preview = new MemePreview(this);
    m_preview->onCopy = [this] {
        if (m_previewIndex >= 0)
            copyEntry(m_previewIndex);
    };
    m_preview->onSaveAs = [this] {
        if (m_previewIndex >= 0)
            saveAs(m_previewIndex);
    };
    // 浮层自己关闭（Esc / 点遮罩 / 关闭按钮）时把网格的播放还回来：
    // 预览期间网格是暂停的，不接这个回调就再也恢复不了。
    m_preview->onClosed = [this] {
        m_previewOpen = false;
        m_previewIndex = -1;
        m_playbackTimer.start();
    };

    // ---- 连线
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.trimmed().isEmpty()) {
            m_debounce.stop();
            resetIdle();
            return;
        }
        m_debounce.start();
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        m_debounce.stop();
        submitSearch(m_search->text());
    });
    connect(m_searchButton, &QPushButton::clicked, this, [this] {
        m_debounce.stop();
        submitSearch(m_search->text());
    });
}

void MemePanel::applyChipStyle()
{
    if (!m_chipRow)
        return;
    // 全局 QPushButton 是 6px 14px 的内边距，套在常用词上会变成一个胖按钮。
    // 用 objectName 覆盖成胶囊形，颜色照样走令牌。
    xtools::theme::applyStyle(
        m_chipRow,
        QStringLiteral("QPushButton#memeChip{background:@bg-subtle;border:1px solid @border-subtle;"
                       "border-radius:12px;padding:2px 12px;min-height:0;min-width:0;"
                       "font-size:12px;color:@text-second;}"
                       "QPushButton#memeChip:hover{background:@bg-hover;"
                       "border-color:@border-hover;color:@text;}"));
}

// ------------------------------------------------------------------ 搜索

void MemePanel::submitSearch(const QString &keyword)
{
    const QString query = keyword.trimmed();
    if (query.isEmpty()) {
        resetIdle();
        return;
    }

    // 防抖定时器与回车可能紧挨着触发同一次搜索，别重复打接口。
    if (query == m_keyword && m_loading)
        return;

    m_keyword = query;
    ++m_generation;         // 旧请求的回调凭这个代际直接丢弃
    abortSearch();

    m_entries.clear();
    m_grid->clearTiles();
    m_grid->setFooter(MemeGrid::Footer::Hidden);
    m_page = 0;
    m_noMore = false;
    m_previewOpen = false;
    m_previewIndex = -1;
    m_preview->closePreview();
    m_source = Source::Fabiaoqing;
    m_fallbackReason.clear();
    // 冷却期内直接开 360：发表情刚刚超时 / 被拦，再等一次 8 秒超时纯属浪费用户时间。
    if (m_fabCooldown.isValid() && m_fabCooldown.elapsed() < kFabCooldownMs) {
        m_source = Source::So360;
        m_fallbackReason = QStringLiteral("刚失败过，冷却期内跳过");
    }
    m_inflight.clear();
    m_downloadQueue.clear();
    m_statusTicket = 0;

    setStatus(QStringLiteral("正在搜索「%1」…").arg(query), xtools::theme::Color::TextFaint);
    requestPage(1);
}

void MemePanel::requestPage(int page)
{
    if (m_keyword.isEmpty())
        return;

    m_loading = true;
    m_grid->setFooter(MemeGrid::Footer::Loading);

    const int generation = m_generation;
    const bool fabStatus = (m_source == Source::Fabiaoqing);

    const QString url = fabStatus
                            ? fabiaoqingSearchUrl(m_keyword, page)
                            : so360SearchUrl(m_keyword, (page - 1) * kSo360PageSize,
                                             kSo360PageSize);
    const QString referer = fabStatus ? QString::fromLatin1(kFabiaoqingReferer)
                                      : QStringLiteral("https://image.so.com/");

    QNetworkReply *reply = m_nam.get(makeRequest(QUrl(url), referer, kSearchTimeoutMs));
    m_searchReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, page] {
        handleSearchReply(reply, generation, page);
    });
}

void MemePanel::abortSearch()
{
    if (m_searchReply) {
        // 断开连接再 abort：abort 会同步触发 finished，不先断开会走一遍"被取消"的错误分支。
        m_searchReply->disconnect(this);
        m_searchReply->abort();
        m_searchReply->deleteLater();
        m_searchReply = nullptr;
    }
    for (QNetworkReply *reply : m_imageReplies) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_imageReplies.clear();
    m_inflight.clear();
    m_activeDownloads = 0;
    if (m_fullReply) {
        m_fullReply->disconnect(this);
        m_fullReply->abort();
        m_fullReply->deleteLater();
        m_fullReply = nullptr;
    }
    m_loading = false;
}

void MemePanel::handleSearchReply(QNetworkReply *reply, int generation, int page)
{
    if (generation != m_generation) {
        reply->deleteLater();
        return;
    }
    m_searchReply = nullptr;
    m_loading = false;

    QVector<MemeItem> items;
    QString error;
    int parsed = -1;

    if (reply->error() != QNetworkReply::NoError) {
        error = describeReplyError(reply);
    } else {
        const QByteArray body = reply->readAll();
        parsed = (m_source == Source::Fabiaoqing)
                     ? parseFabiaoqingPage(body, &items, &error)
                     : parseSo360Json(body, &items, &error);
    }
    // 内容都取完了再释放：先 deleteLater 再 readAll 虽然当前还能用，
    // 但依赖"事件循环还没轮到"的隐性时序，改一次顺序就会踩空。
    reply->deleteLater();

    if (parsed > 0) {
        // 主源通了就取消冷却，别让它一直挂着。
        if (m_source == Source::Fabiaoqing)
            m_fabCooldown.invalidate();
        applyResults(items, page);
        return;
    }

    const QString reason = !error.isEmpty() ? error : QStringLiteral("没有找到相关表情");

    if (page > 1) {
        // 后续页空了只表示翻到底了，不该再换源，否则会从头再列一遍。
        m_noMore = true;
        m_grid->setFooter(MemeGrid::Footer::NoMore);
        setStatus(QStringLiteral("没有更多了（共 %1 个结果）").arg(m_entries.size()),
                  xtools::theme::Color::TextFaint);
        return;
    }

    if (m_source == Source::Fabiaoqing) {
        // 只有「真失败」才冷却：解析出 0 条说明站点是通的，只是这个词没结果，
        // 不能因此把主源停掉 5 分钟。
        if (!error.isEmpty() || parsed < 0)
            m_fabCooldown.restart();
        switchToSo360(reason);
        return;
    }

    // 两家都空/都失败：把两家的情况都说出来，不静默。
    m_noMore = true;
    m_grid->setFooter(MemeGrid::Footer::Hidden);
    m_grid->setEmptyText(QStringLiteral("没有找到「%1」相关表情").arg(m_keyword),
                         QStringLiteral("换个关键词试试，或者点上面的常用词"));
    m_grid->update();
    setStatus(QStringLiteral("搜索失败：%1；360 图片：%2")
                  .arg(m_fallbackReason.isEmpty() ? QStringLiteral("无结果") : m_fallbackReason,
                       reason),
              xtools::theme::Color::Danger);
    m_count->setText(QString());
}

void MemePanel::switchToSo360(const QString &reason)
{
    m_fallbackReason = reason;
    m_source = Source::So360;
    m_page = 0;
    m_noMore = false;
    m_entries.clear();
    m_grid->clearTiles();
    m_grid->setFooter(MemeGrid::Footer::Hidden);
    setStatus(QStringLiteral("发表情：%1，改用 360 图片").arg(reason),
              xtools::theme::Color::Warn);
    requestPage(1);
}

void MemePanel::applyResults(const QVector<MemeItem> &items, int page)
{
    m_page = page;

    // 360 会无视 pn 一次返回近百条，截断到和发表情一个量级，
    // 「第 N 页」的含义才不会一会儿 15 条一会儿 97 条。
    const int limit = (m_source == Source::So360) ? kSo360MaxPerPage : items.size();
    int added = 0;
    for (const MemeItem &item : items) {
        if (added >= limit)
            break;
        if (item.thumbUrl.isEmpty())
            continue;

        const int index = m_entries.size();

        Entry entry;
        entry.item = item;
        entry.keyThumb = cacheKey(item.thumbUrl);
        m_entries.append(entry);

        MemeTile *tile = m_grid->addTile(item);
        m_entries[index].tile = tile;

        // 索引是在追加时确定的，回调里直接按索引找回条目 —— 瓦片不知道自己在哪一页。
        tile->onCopy = [this](int i) { copyEntry(i); };
        tile->onPreview = [this](int i) { showPreview(i); };
        tile->onContextMenu = [this](int i, const QPoint &pos) { showContextMenu(i, pos); };

        queueThumb(index);
        ++added;
    }

    if (added == 0) {
        m_noMore = true;
        m_grid->setFooter(MemeGrid::Footer::NoMore);
        return;
    }

    m_grid->setFooter(MemeGrid::Footer::LoadMore);
    m_grid->update();

    const QString sourceName = (m_source == Source::Fabiaoqing) ? QStringLiteral("发表情")
                                                                : QStringLiteral("360 图片");
    QString text = QStringLiteral("来源：%1 · 第 %2 页").arg(sourceName).arg(m_page);
    if (!m_fallbackReason.isEmpty() && m_source == Source::So360)
        text += QStringLiteral("（发表情：%1）").arg(m_fallbackReason);
    setStatus(text, xtools::theme::Color::TextFaint);
    m_count->setText(QStringLiteral("共 %1 个").arg(m_entries.size()));
}

void MemePanel::resetIdle()
{
    ++m_generation;
    abortSearch();

    m_keyword.clear();
    m_page = 0;
    m_noMore = false;
    m_fallbackReason.clear();
    m_source = Source::Fabiaoqing;
    m_entries.clear();
    m_grid->clearTiles();
    m_grid->setEmptyText(QStringLiteral("输入关键词搜索表情包"),
                         QStringLiteral("单击复制到剪贴板，双击或点角标看大图"));
    m_grid->update();
    m_grid->setFooter(MemeGrid::Footer::Hidden);
    m_count->clear();
    m_previewOpen = false;
    m_previewIndex = -1;
    m_preview->closePreview();
    m_statusTicket = 0;
    restoreBaseStatus();
}

// ------------------------------------------------------------------ 图片

void MemePanel::queueThumb(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;

    Entry &entry = m_entries[index];
    if (entry.item.thumbUrl.isEmpty())
        return;

    entry.requested = true;

    const QString hit = m_cache.find(entry.keyThumb);
    if (!hit.isEmpty()) {
        entry.path = hit;
        loadPoster(index);
        return;
    }

    // 同一个 URL 可能在不同页重复出现；已在下载中就等那一份的回填。
    if (m_inflight.contains(entry.keyThumb))
        return;

    if (!m_downloadQueue.contains(entry.item.thumbUrl))
        m_downloadQueue.enqueue(entry.item.thumbUrl);
    pumpDownloads();
}

void MemePanel::pumpDownloads()
{
    while (m_activeDownloads < kMaxConcurrentDownloads && !m_downloadQueue.isEmpty()) {
        const QString url = m_downloadQueue.dequeue();
        const QString key = cacheKey(url);
        if (m_inflight.contains(key))
            continue;

        const QString cached = m_cache.find(key);
        if (!cached.isEmpty()) {
            assignImage(key, cached, QString());
            continue;
        }

        // Referer 跟着条目走：发表情必须带，360 不需要（带了也无害）。
        QString referer;
        for (const Entry &entry : m_entries) {
            if (entry.keyThumb == key) {
                referer = entry.item.referer;
                break;
            }
        }

        QNetworkReply *reply =
            m_nam.get(makeRequest(QUrl(url), referer, kImageTimeoutMs));
        m_inflight.insert(key);
        m_imageReplies.insert(key, reply);
        ++m_activeDownloads;

        connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
            handleImageReply(reply, key);
        });
        // 体积守卫：超限直接 abort，回调会以错误收场，不会把几十兆读进内存。
        connect(reply, &QNetworkReply::downloadProgress, this,
                [reply](qint64 received, qint64) {
                    if (received > kMaxImageBytes)
                        reply->abort();
                });
    }
}

void MemePanel::handleImageReply(QNetworkReply *reply, const QString &key)
{
    m_imageReplies.remove(key);
    m_inflight.remove(key);
    if (m_activeDownloads > 0)
        --m_activeDownloads;

    QString path;
    QString failure;

    if (reply->error() != QNetworkReply::NoError) {
        failure = describeReplyError(reply);
    } else {
        const QByteArray bytes = reply->readAll();
        path = m_cache.store(key, bytes, &failure);
        if (!path.isEmpty() && ++m_storedSincePrune >= kPruneEveryStores) {
            m_storedSincePrune = 0;
            m_cache.prune(kCacheMaxFiles, kCacheMaxBytes);
        }
    }
    reply->deleteLater();

    assignImage(key, path, failure);
    pumpDownloads();
}

void MemePanel::assignImage(const QString &key, const QString &path, const QString &failure)
{
    // 同一 URL 可能被多个条目引用（跨页重复），统一按 key 回填，不必维护等待者列表。
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].keyThumb != key)
            continue;

        if (!path.isEmpty()) {
            m_entries[i].path = path;
            loadPoster(i);
            continue;
        }

        m_entries[i].failed = true;
        m_entries[i].copyWhenReady = false;
        if (m_entries[i].tile)
            m_entries[i].tile->setFailure(failure.isEmpty()
                                              ? QStringLiteral("图片加载失败")
                                              : failure);
    }
}

void MemePanel::loadPoster(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;

    Entry &entry = m_entries[index];
    if (entry.path.isEmpty() || !entry.tile)
        return;

    const qreal dpr = devicePixelRatioF();
    const int edge = qRound(MemeTile::kImageSize * dpr);

    QImageReader reader(entry.path);
    // 先按目标尺寸解码，而不是读完再缩：一张 1200×1200 的图整读进来是 5.7MB，
    // 45 张就是 250MB，直接把内存吃光。
    const QSize source = reader.size();
    if (source.isValid() && !source.isEmpty())
        reader.setScaledSize(source.scaled(QSize(edge, edge), Qt::KeepAspectRatio));

    const QImage image = reader.read();
    if (image.isNull()) {
        entry.failed = true;
        entry.tile->setFailure(QStringLiteral("图片解码失败"));
        return;
    }

    QPixmap poster = QPixmap::fromImage(image);
    poster.setDevicePixelRatio(dpr);

    entry.animated = reader.supportsAnimation() && reader.imageCount() > 1;
    entry.ready = true;
    entry.failed = false;
    entry.tile->setPoster(poster, entry.animated);

    if (entry.copyWhenReady) {
        entry.copyWhenReady = false;
        performCopy(index, entry.path);
    }

    m_playbackTimer.start();
}

void MemePanel::requestFullImage(int index)
{
    if (index < 0 || index >= m_entries.size() || m_fullReply)
        return;

    Entry &entry = m_entries[index];
    if (entry.item.fullUrl.isEmpty())
        return;

    // 预览上那行元信息：格式 + 实际体积。体积取的是将要显示的那个文件。
    const auto show = [this, &entry](const QString &path) {
        m_preview->setImagePath(path, entry.animated,
                                QStringLiteral("%1 · %2")
                                    .arg(entry.animated ? QStringLiteral("GIF 动图")
                                                        : QStringLiteral("静态图片"),
                                         meme::humanByteSize(QFileInfo(path).size())));
    };

    entry.keyFull = cacheKey(entry.item.fullUrl);
    // 发表情的 thumb 与 full 是同一个 URL（实测 bmiddle/large 字节一致），
    // 这种情况直接用已下载的缩略图，没必要再下第二遍。
    if (entry.keyFull == entry.keyThumb && !entry.path.isEmpty()) {
        entry.fullPath = entry.path;
        show(entry.fullPath);
        return;
    }

    const QString hit = m_cache.find(entry.keyFull);
    if (!hit.isEmpty()) {
        entry.fullPath = hit;
        show(hit);
        return;
    }

    QNetworkRequest request =
        makeRequest(QUrl(entry.item.fullUrl), entry.item.referer, kImageTimeoutMs);
    QNetworkReply *reply = m_nam.get(request);
    m_fullReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, index] {
        handleFullReply(reply, index);
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply](qint64 received, qint64) {
                if (received > kMaxImageBytes)
                    reply->abort();
            });
}

void MemePanel::handleFullReply(QNetworkReply *reply, int index)
{
    m_fullReply = nullptr;
    const bool stillWanted = (index >= 0 && index < m_entries.size() && m_previewIndex == index);

    // 先把内容和错误文案取出来，再放掉 reply（理由同 handleSearchReply）。
    const QNetworkReply::NetworkError status = reply->error();
    const QString reason = describeReplyError(reply);
    const QByteArray body = (status == QNetworkReply::NoError) ? reply->readAll() : QByteArray();
    reply->deleteLater();

    if (!stillWanted)
        return;   // 用户已经关掉或切到别的图了

    Entry &entry = m_entries[index];

    if (status != QNetworkReply::NoError) {
        m_preview->setImagePath(QString(),
                                entry.animated,
                                QStringLiteral("原图取不到：%1，下面显示的是缩略图").arg(reason));
        return;
    }

    QString error;
    const QString path = m_cache.store(entry.keyFull, body, &error);
    if (path.isEmpty()) {
        m_preview->setImagePath(QString(), entry.animated,
                                QStringLiteral("原图存不下来：%1").arg(error));
        return;
    }

    entry.fullPath = path;
    m_preview->setImagePath(path, entry.animated,
                            QStringLiteral("%1 · %2")
                                .arg(entry.animated ? QStringLiteral("GIF 动图")
                                                    : QStringLiteral("静态图片"),
                                     meme::humanByteSize(QFileInfo(path).size())));
}

// ------------------------------------------------------------------ 播放

QSize MemePanel::movieTarget(const Entry &entry) const
{
    const qreal dpr = devicePixelRatioF();
    const int edge = qRound(MemeTile::kImageSize * dpr);

    QImageReader reader(entry.path);
    QSize source = reader.size();
    if (!source.isValid() || source.isEmpty())
        return QSize(edge, edge);

    const QSize fitted = source.scaled(QSize(edge, edge), Qt::KeepAspectRatio);
    return fitted.isEmpty() ? QSize(edge, edge) : fitted;
}

void MemePanel::refreshPlayback()
{
    if (m_previewOpen || !m_scroll)
        return;

    QWidget *viewport = m_scroll->viewport();
    const QRect view = viewport->rect();

    // 按「离视口中心的距离」排序，名额不够时优先播最中间的。
    QVector<QPair<int, int>> candidates;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &entry = m_entries[i];
        if (!entry.tile || !entry.ready || !entry.animated || entry.path.isEmpty())
            continue;

        const QPoint topLeft = entry.tile->mapTo(viewport, QPoint(0, 0));
        const QRect rect(topLeft, entry.tile->size());
        if (!rect.intersects(view))
            continue;
        candidates.append({qAbs(rect.center().y() - view.center().y()), i});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const QPair<int, int> &a, const QPair<int, int> &b) {
                  return a.first < b.first;
              });

    QSet<int> want;
    for (const auto &candidate : candidates) {
        if (want.size() >= kMaxPlayingMovies)
            break;
        want.insert(candidate.second);
    }

    for (int i = 0; i < m_entries.size(); ++i) {
        Entry &entry = m_entries[i];
        if (!entry.tile)
            continue;

        if (want.contains(i)) {
            if (!entry.playing) {
                entry.tile->startMovie(entry.path, movieTarget(entry));
                entry.playing = entry.tile->isPlaying();
            }
        } else if (entry.playing) {
            // 滚出视口就立刻停：40 个 QMovie 同时跑会持续占用 GUI 线程，界面明显发涩。
            entry.tile->stopMovie();
            entry.playing = false;
        }
    }
}

void MemePanel::stopAllPlayback()
{
    for (Entry &entry : m_entries) {
        if (!entry.tile || !entry.playing)
            continue;
        entry.tile->stopMovie();
        entry.playing = false;
    }
}

// ------------------------------------------------------------------ 交互

void MemePanel::showContextMenu(int index, const QPoint &globalPos)
{
    if (index < 0 || index >= m_entries.size())
        return;

    const Entry &entry = m_entries[index];

    QMenu menu(this);
    QAction *copyAction = menu.addAction(QStringLiteral("复制图片"));
    QAction *previewAction = menu.addAction(QStringLiteral("查看大图"));
    menu.addSeparator();
    QAction *saveAction = menu.addAction(QStringLiteral("另存为…"));
    saveAction->setEnabled(!(entry.path.isEmpty() && entry.fullPath.isEmpty()));
    QAction *openAction = nullptr;
    if (!entry.item.detailUrl.isEmpty()) {
        menu.addSeparator();
        openAction = menu.addAction(QStringLiteral("在浏览器打开原网页"));
    }

    QAction *chosen = menu.exec(globalPos);
    if (!chosen)
        return;

    if (chosen == copyAction)
        copyEntry(index);
    else if (chosen == previewAction)
        showPreview(index);
    else if (chosen == saveAction)
        saveAs(index);
    else if (openAction && chosen == openAction)
        QDesktopServices::openUrl(QUrl(entry.item.detailUrl));
}

void MemePanel::showPreview(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;

    const Entry &entry = m_entries[index];
    m_previewIndex = index;
    m_previewOpen = true;

    // 预览时把网格里的动图全停掉：同一个面板里同时跑十几个播放器没有必要。
    stopAllPlayback();

    // 原图还没下来就先用缩略图顶上，同时后台取原图（360 的缩略图只有 240px）。
    const QString initial = entry.fullPath.isEmpty() ? entry.path : entry.fullPath;
    const QString meta = QStringLiteral("%1 · %2")
                             .arg(entry.animated ? QStringLiteral("GIF 动图")
                                                 : QStringLiteral("静态图片"),
                                  initial.isEmpty()
                                      ? QStringLiteral("正在加载…")
                                      : meme::humanByteSize(QFileInfo(initial).size()));
    m_preview->openItem(entry.item, initial, entry.animated, meta,
                        QStringLiteral("正在加载…"));

    requestFullImage(index);
}

void MemePanel::copyEntry(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;

    Entry &entry = m_entries[index];

    if (!entry.path.isEmpty()) {
        performCopy(index, entry.path);
        return;
    }

    if (entry.failed) {
        setTransientStatus(QStringLiteral("这张图没下载成功，换一张试试"),
                           xtools::theme::Color::Danger);
        return;
    }

    // 图片还在路上：记下"到了就复制"，比让用户再点一次更符合预期。
    entry.copyWhenReady = true;
    if (!entry.requested)
        queueThumb(index);
    setTransientStatus(QStringLiteral("图片还在下载，完成后会自动复制"),
                       xtools::theme::Color::TextFaint);
}

void MemePanel::performCopy(int index, const QString &path)
{
    if (index < 0 || index >= m_entries.size())
        return;

    const Entry &entry = m_entries[index];

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setTransientStatus(QStringLiteral("读取缓存失败：%1").arg(file.errorString()),
                           xtools::theme::Color::Danger);
        return;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    // QImage 读 GIF 得到的是首帧：剪贴板的静态兜底就靠它。
    QImage firstFrame(path);
    if (firstFrame.isNull()) {
        setTransientStatus(QStringLiteral("图片解码失败，可以先试试另存为"),
                           xtools::theme::Color::Danger);
        return;
    }

    // 文件通道指向 outbox 里的稳定副本：缓存可能几分钟后被清理，
    // 剪贴板里的路径届时不能失效。
    const QString stable = m_cache.outboxFile(entry.keyThumb, path);
    const QString forClipboard = stable.isEmpty() ? path : stable;

    QMimeData *mime =
        buildClipboardMime(bytes, firstFrame, forClipboard, entry.animated);
    QGuiApplication::clipboard()->setMimeData(mime);         // 所有权交给剪贴板

    if (!entry.animated) {
        setTransientStatus(QStringLiteral("已复制图片到剪贴板"), xtools::theme::Color::Success);
        return;
    }

#if defined(Q_OS_MACOS)
    setTransientStatus(QStringLiteral("已复制首帧与 GIF 文件地址；macOS 剪贴板没有通用动图类型"),
                       xtools::theme::Color::Success);
#else
    setTransientStatus(
        QStringLiteral("已复制动图；若粘贴后不动，可另存为 GIF 后按文件发送"),
        xtools::theme::Color::Success);
#endif
}

void MemePanel::saveAs(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;

    const Entry &entry = m_entries[index];
    QString source = entry.fullPath.isEmpty() ? entry.path : entry.fullPath;
    if (source.isEmpty()) {
        setTransientStatus(QStringLiteral("图片还没下载好，稍等一下再另存"),
                           xtools::theme::Color::Warn);
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();

    const QFileInfo info(source);
    const QString suggested =
        QDir(dir).filePath(QStringLiteral("表情_%1.%2")
                               .arg(entry.keyThumb.left(8), info.suffix()));
    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存表情包"), suggested,
        QStringLiteral("图片 (*.gif *.jpg *.png *.bmp)"));
    if (target.isEmpty())
        return;

    // QFile::copy 不覆盖已存在的目标，先删再拷。
    QFile::remove(target);
    if (QFile::copy(source, target))
        setTransientStatus(QStringLiteral("已保存到 %1").arg(target),
                           xtools::theme::Color::Success);
    else
        setTransientStatus(QStringLiteral("保存失败，换个目录试试"), xtools::theme::Color::Danger);
}

// ------------------------------------------------------------------ 状态行

void MemePanel::setStatus(const QString &text, xtools::theme::Color color)
{
    m_baseStatus = text;
    m_baseStatusColor = color;

    xtools::theme::applyStyle(m_status,
                              QStringLiteral("font-size:12px;color:@%1;")
                                  .arg(xtools::theme::tokenName(color)));

    // 状态行是单行，长文案（比如两家都失败的组合原因）必须省略，完整内容放 tooltip。
    const QFontMetrics metrics(m_status->font());
    const int available = qMax(80, m_status->width());
    m_status->setText(metrics.elidedText(text, Qt::ElideRight, available));
    m_status->setToolTip(text);
}

void MemePanel::setTransientStatus(const QString &text, xtools::theme::Color color)
{
    const int ticket = ++m_statusTicket;
    setStatus(text, color);

    // 2.5 秒后回到搜索状态；期间又弹了别的提示就作废这一条（凭 ticket 判断）。
    QTimer::singleShot(2500, this, [this, ticket] {
        if (ticket != m_statusTicket)
            return;
        m_statusTicket = 0;
        setStatus(m_baseStatus, m_baseStatusColor);
    });
}

void MemePanel::restoreBaseStatus()
{
    if (m_baseStatus.isEmpty()) {
        m_baseStatus = QStringLiteral("输入关键词搜索，单击缩略图即复制到剪贴板");
        m_baseStatusColor = xtools::theme::Color::TextFaint;
    }
    m_statusTicket = 0;
    setStatus(m_baseStatus, m_baseStatusColor);
}

// ------------------------------------------------------------------ 尺寸

void MemePanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    // 高度不够时收起常用词，把 34px 让给网格；独立窗口最小 560×420 时会走到这里。
    if (m_chipRow)
        m_chipRow->setVisible(height() >= kChipsMinHeight);

    if (m_preview) {
        m_preview->setGeometry(rect());
        if (m_previewOpen)
            m_preview->raise();
    }

    // 列数会变，重新算一次状态行的可用宽度（用最新的宽度做省略）。
    if (m_status)
        setStatus(m_baseStatus, m_baseStatusColor);

    m_playbackTimer.start();
}

void MemePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_playbackTimer.start();
}

void MemePanel::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // 面板被切走还让 GIF 继续解码是纯浪费（宿主回首页/进设置都会触发）。
    stopAllPlayback();
}

} // namespace meme

#pragma once

// ---------------------------------------------------------------------------
// 表情包搜索面板：搜索栏 + 常用词 + 瓦片网格 + 状态行 + 预览浮层。
//
// 这是插件的总装层，负责所有「有状态、会失败」的东西：
//   · 搜索请求与两家数据源的降级（发表情 → 360 图片）
//   · 图片下载队列（并发上限、同一 URL 去重）
//   · 磁盘缓存与「点击时图片还没下载完」的延迟复制
//   · 视口内动图播放调度（只播看得见的，上限 16 个）
//   · 剪贴板写入（三端格式不同，见 copyEntry 的注释）
//
// 宿主契约（docs/PLUGIN.md）：面板每次进入工具都会重新创建，也可能同时存在
// 「内嵌」与「独立窗口」两份。所以**所有状态都放在这个实例的成员里**，
// 不能有静态/全局可变状态，否则两份面板会互相踩。
//
// 刻意不带 Q_OBJECT：不需要信号槽，连接全部用 lambda + 成员上下文，
// 省掉 moc；临时测试工程也能直接编这个 .cpp。
// ---------------------------------------------------------------------------

#include "MemeCache.h"
#include "MemeGrid.h"
#include "MemePreview.h"
#include "MemeSource.h"
#include "ThemePalette.h"   // 状态行的颜色令牌

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QMimeData;
class QNetworkReply;
class QPushButton;
class QScrollArea;

namespace meme {

/// 组装「复制图片」用的剪贴板数据。三端能接受的格式不同（详见实现里的注释），
/// 这里一次性把三种都写进去，尽量让动图活下来。
///
/// 单独抽成自由函数的理由是可验证性：无桌面/受限环境下 QClipboard 本身可能
/// 读写不通（实测沙箱里连 setText 都读不回来），把"组装"和"上剪贴板"拆开，
/// 就能在不碰系统剪贴板的前提下断言该写的格式都写了。
/// 返回值所有权交给调用方（setMimeData 会接管）。
QMimeData *buildClipboardMime(const QByteArray &bytes, const QImage &firstFrame,
                              const QString &filePath, bool animated);

class MemePanel : public QWidget
{
public:
    explicit MemePanel(QWidget *parent = nullptr);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    enum class Source
    {
        Fabiaoqing,
        So360,
    };

    /// 一条结果在面板里的全部状态。tile 归 MemeGrid 所有，这里只借用。
    struct Entry
    {
        MemeItem item;
        QString keyThumb;
        QString keyFull;
        QString path;        ///< 缩略图在本地的路径（空 = 还没下载好）
        QString fullPath;    ///< 原图本地路径（发表情与缩略图相同，只有 360 需要单独取）
        bool animated = false;
        bool ready = false;      ///< 海报已解码、可以显示
        bool playing = false;    ///< 当前有 QMovie 在跑
        bool requested = false;  ///< 已经发过下载请求
        bool failed = false;
        bool copyWhenReady = false;   ///< 用户点了但图还没到，落盘后自动补一次复制
        MemeTile *tile = nullptr;
    };

    // ---- 界面搭建
    void buildUi();
    void applyChipStyle();

    // ---- 搜索
    void submitSearch(const QString &keyword);
    void requestPage(int page);
    void abortSearch();
    void handleSearchReply(QNetworkReply *reply, int generation, int page);
    void applyResults(const QVector<MemeItem> &items, int page);
    void switchToSo360(const QString &reason);
    void resetIdle();

    // ---- 图片
    void queueThumb(int index);
    void pumpDownloads();
    void handleImageReply(QNetworkReply *reply, const QString &key);
    void assignImage(const QString &key, const QString &path, const QString &failure);
    void loadPoster(int index);
    void requestFullImage(int index);
    void handleFullReply(QNetworkReply *reply, int index);

    // ---- 播放调度
    void refreshPlayback();
    void stopAllPlayback();
    QSize movieTarget(const Entry &entry) const;

    // ---- 交互
    void showContextMenu(int index, const QPoint &globalPos);
    void showPreview(int index);
    void copyEntry(int index);
    void performCopy(int index, const QString &path);
    void saveAs(int index);

    // ---- 状态行
    void setStatus(const QString &text, xtools::theme::Color color);
    void setTransientStatus(const QString &text, xtools::theme::Color color);
    void restoreBaseStatus();

    Cache m_cache;
    QNetworkAccessManager m_nam;
    QVector<Entry> m_entries;

    QString m_keyword;
    Source m_source = Source::Fabiaoqing;
    QString m_fallbackReason;   ///< 从发表情降级时记下原因，状态栏要显示
    /// 发表情真失败（超时 / 403 / 页面结构变了）后冷却一段时间，期间直接走 360。
    /// 站点的软封禁一口气能持续几分钟，每次都先等满 8 秒超时才出结果太难受。
    QElapsedTimer m_fabCooldown;
    int m_page = 0;
    int m_generation = 0;
    bool m_loading = false;
    bool m_noMore = false;

    QNetworkReply *m_searchReply = nullptr;
    QNetworkReply *m_fullReply = nullptr;

    QSet<QString> m_inflight;
    QHash<QString, QNetworkReply *> m_imageReplies;
    QQueue<QString> m_downloadQueue;   ///< 待下载的 URL
    int m_activeDownloads = 0;
    int m_storedSincePrune = 0;

    int m_previewIndex = -1;
    bool m_previewOpen = false;

    QString m_baseStatus;
    xtools::theme::Color m_baseStatusColor = xtools::theme::Color::TextFaint;
    int m_statusTicket = 0;

    QLineEdit *m_search = nullptr;
    QPushButton *m_searchButton = nullptr;
    QWidget *m_chipRow = nullptr;
    QScrollArea *m_scroll = nullptr;
    MemeGrid *m_grid = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_count = nullptr;
    MemePreview *m_preview = nullptr;

    QTimer m_debounce;
    QTimer m_playbackTimer;

    /// 同时播放的动图上限。主窗口 5 列 × 3 行 = 15，独立窗口 5 列 × 2 行 = 10，
    /// 16 足够覆盖常规视口；更大的屏只播离视口中心最近的 16 个，其余的显示首帧。
    static constexpr int kMaxPlayingMovies = 16;
    /// 同时进行的图片下载数。缩略图只有几十 KB，3 个并发足够快，又不至于被图床限流。
    static constexpr int kMaxConcurrentDownloads = 3;
    /// 360 接口会无视 pn 参数一次返回近百条，这里截断，保证「一页」概念一致。
    static constexpr int kSo360PageSize = 24;
    static constexpr int kSo360MaxPerPage = 24;
};

} // namespace meme

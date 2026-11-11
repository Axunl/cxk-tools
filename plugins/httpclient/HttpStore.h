#pragma once

// ---------------------------------------------------------------------------
// 请求的本地存档：草稿 / 集合 / 历史。
//
// 为什么不用 QSettings/INI：一条请求里有参数、请求头两层数组，INI 写起来要
// beginWriteArray 套 beginWriteArray，读回来还容易残留旧下标。整份存档就是
// 一个小 JSON，加载一次全量、保存一次全量，代码短且不会出现「删了条目还在」。
//
// 文件位置走 QStandardPaths::AppConfigLocation，与 jsrun 的存档同目录：
//   Windows  %APPDATA%/.../cxk-tools/httpclient.json
//   macOS    ~/Library/Preferences/.../httpclient.json
//   Linux    ~/.config/.../httpclient.json
// 目录不存在时先 mkpath —— QSaveFile 不会替你建父目录，会静默失败。
// ---------------------------------------------------------------------------

#include "HttpSpec.h"

#include <QDateTime>
#include <QString>
#include <QVector>

namespace httpapi {

/// 历史条目：请求本身 + 发送时间（侧栏显示「几分钟前」）。
struct HistoryItem
{
    Spec      spec;
    QDateTime at;
};

class Store
{
public:
    /// path 为空时用默认位置。测试可以传临时文件路径，避免动到用户配置。
    explicit Store(QString path = QString());

    QString filePath() const { return m_path; }

    /// 上次正在编辑的请求（面板每次进入都会重建，靠它恢复现场）。
    Spec draft() const;
    void setDraft(const Spec &spec);

    /// 集合（用户主动保存的请求），按保存顺序排列，不自动排序。
    QVector<Spec> saved() const;
    void setSaved(const QVector<Spec> &items);

    /// 历史：最近发送的在前。
    QVector<HistoryItem> history() const;
    void setHistory(const QVector<HistoryItem> &items);

    /// 追加一条历史：同方法 + 同地址的旧记录被提到最前（不重复堆叠），
    /// 超过 max 条从尾部丢弃。历史里只存请求形状，不含响应。
    void addHistory(const Spec &spec, int max = 30);

private:
    QJsonObject load() const;
    void        save(const QJsonObject &root) const;

    QString m_path;
};

} // namespace httpapi

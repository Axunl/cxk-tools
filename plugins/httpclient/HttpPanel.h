#pragma once

// ---------------------------------------------------------------------------
// API 调试面板：Postman 的核心链路（方法 + 地址 + 参数/请求头/请求体/认证 +
// 响应查看 + 集合/历史 + cURL 互转），但所有状态都留在本面板实例里。
//
// 面板会被宿主反复创建/销毁（切页即销毁，见 docs/PLUGIN.md），所以：
//   * 不持有任何静态 UI 状态；
//   * 编辑中的请求靠 Store 的「草稿」跨面板存活；
//   * 发送中的 reply 在析构时先断连再 abort，避免 finished 回调打到半析构对象。
// ---------------------------------------------------------------------------

#include "HttpSpec.h"
#include "HttpStore.h"

#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QWidget>

class ElideLabel;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTabWidget;
class QTimer;

class HttpPanel : public QWidget
{
    Q_OBJECT

public:
    explicit HttpPanel(QWidget *parent = nullptr);
    ~HttpPanel() override;

private:
    // ---- 搭界面（拆成小块，避免一个函数几百行）
    void     buildUi();
    QWidget *buildSidebar();
    QWidget *buildRequestArea();
    QWidget *buildParamsTab();
    QWidget *buildHeadersTab();
    QWidget *buildBodyTab();
    QWidget *buildAuthTab();
    QWidget *buildResponseArea();

    // ---- 请求 ↔ 界面
    httpapi::Spec gatherSpec() const;
    void          applySpec(const httpapi::Spec &spec);
    void          refreshParamsFromUrl();
    void          rewriteUrlFromParams();
    void          updateTabLabels();
    void          updateBodyEditor();
    void          updateAuthPage();

    // ---- 集合 / 历史
    void refreshSavedList();
    void refreshHistoryList();
    void saveCurrentToCollection();
    void deleteSavedItem(int row);
    void deleteHistoryItem(int row);
    void clearHistory();
    void loadListItem(QListWidgetItem *item);
    void showListMenu(const QPoint &pos, bool saved);

    // ---- 发送与响应
    void send();
    void onReplyFinished();
    void renderResponse();
    void fillResponseHeaders(const QList<QPair<QByteArray, QByteArray>> &headers);
    void resetResponseView(const QString &placeholder);

    // ---- 小工具
    void setHint(const QString &text, bool error = false);
    void setStatus(const QString &text, int statusCode);
    void scheduleDraftSave();
    void saveDraftNow();
    void copyResponse();
    void copyCurl();
    void importCurl();

    // ---- 数据
    httpapi::Store m_store;

    QComboBox      *m_method = nullptr;
    QLineEdit      *m_url = nullptr;
    QPushButton    *m_send = nullptr;
    QTabWidget     *m_reqTabs = nullptr;

    QTableWidget   *m_params = nullptr;
    QTableWidget   *m_headers = nullptr;

    QComboBox      *m_bodyMode = nullptr;
    QPlainTextEdit *m_body = nullptr;
    QPushButton    *m_formatJson = nullptr;

    QComboBox      *m_authMode = nullptr;
    QStackedWidget *m_authStack = nullptr;
    QLineEdit      *m_token = nullptr;
    QLineEdit      *m_user = nullptr;
    QLineEdit      *m_password = nullptr;
    QLineEdit      *m_keyName = nullptr;
    QLineEdit      *m_keyValue = nullptr;
    QComboBox      *m_keyIn = nullptr;

    QListWidget    *m_savedList = nullptr;
    QListWidget    *m_historyList = nullptr;
    QLabel         *m_savedEmpty = nullptr;
    QLabel         *m_historyEmpty = nullptr;

    ElideLabel     *m_status = nullptr;
    QTabWidget     *m_respTabs = nullptr;
    QPlainTextEdit *m_respBody = nullptr;
    QTableWidget   *m_respHeaders = nullptr;
    QCheckBox      *m_pretty = nullptr;
    QLabel         *m_hint = nullptr;

    // ---- 网络
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply         *m_reply = nullptr;
    QElapsedTimer          m_elapsed;
    httpapi::Spec          m_pendingSpec;   ///< 发送那一刻的快照，进历史用

    // ---- 状态
    bool    m_syncing = false;      ///< 参数表 ↔ 地址栏同步时抑制回调，避免互相触发
    QString m_currentName;          ///< 当前对应的集合项名；草稿/历史为空
    QByteArray m_respRaw;           ///< 原始响应体（切「美化 JSON」时重新渲染）
    QString m_respContentType;
    bool    m_respIsJson = false;

    QTimer *m_draftTimer = nullptr;
};

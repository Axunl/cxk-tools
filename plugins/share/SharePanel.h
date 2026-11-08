#pragma once

// ---------------------------------------------------------------------------
// 文件共享工具的主面板。
//
// 面板只是视图：共享列表与 HTTP 服务都在插件模块的进程级单例里（见 ShareStore.h
// 与 ShareServer.h 的说明）。所以关掉面板、切到别的工具时服务继续跑，
// 重新进入能看到同一份列表 —— 这是这个工具与仓库其他工具最大的行为差异。
// ---------------------------------------------------------------------------

#include <QList>
#include <QStringList>
#include <QWidget>

#include "ShareHttp.h"   // LocalAddress 要作为 QList 成员，必须完整类型（前向声明不够）

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QToolButton;
class QTreeWidget;

class SharePanel : public QWidget
{
    Q_OBJECT

public:
    explicit SharePanel(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QWidget *buildAddressCard();
    QWidget *buildFileCard();
    QWidget *buildTextCard();

    void startServer();
    /// 重新枚举网卡。force=false 时地址没变就不动界面，避免每 5 秒打断用户选择。
    void refreshAddress(bool force);
    void rebuildAddressMenu();
    void refreshFiles();
    void refreshTexts();

    void addFilesDialog();
    void addFolderDialog();
    void addLocalPaths(const QStringList &paths);
    void removeSelectedFile();
    void removeSelectedText();
    void clearFiles();
    void sendText();
    void copyLink();
    void openInBrowser();

    void setStatus(const QString &text, bool error = false);
    QString currentUrl() const;

    // ---- 地址区 ----
    QLabel *m_dot = nullptr;
    QLabel *m_badge = nullptr;
    QLineEdit *m_urlEdit = nullptr;
    QPushButton *m_switchBtn = nullptr;
    QPushButton *m_copyBtn = nullptr;
    QPushButton *m_openBtn = nullptr;

    // ---- 文件区 ----
    QTreeWidget *m_fileTree = nullptr;
    QLabel *m_fileCount = nullptr;
    QPushButton *m_fileRemoveBtn = nullptr;
    QPushButton *m_fileClearBtn = nullptr;

    // ---- 文本区 ----
    QListWidget *m_textList = nullptr;
    QLineEdit *m_textInput = nullptr;
    QPushButton *m_textRemoveBtn = nullptr;
    QPushButton *m_textClearBtn = nullptr;
    QLabel *m_textCount = nullptr;

    QLabel *m_status = nullptr;
    QTimer *m_addressTimer = nullptr;

    /// 当前枚举到的地址，以及用户在多网卡时手动选中的那个。
    QList<share::LocalAddress> m_addresses;
    QString m_selectedUrl;
    /// 上一次刷新时的地址快照，用来判断"网卡没变就别动界面"。
    QStringList m_knownUrls;
};

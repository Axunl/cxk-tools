#pragma once

// ---------------------------------------------------------------------------
// 面板内的预览浮层（遮罩 + 居中卡片），不是独立窗口。
//
// 为什么不做成顶层窗口：AGENTS.md 明确要求插件不要自己弹窗（宿主已有「独立窗口」
// 按钮）；做成子控件还省掉了一整套窗口生命周期管理，内嵌与独立窗口两种宿主形态
// 下表现完全一致，Esc / 点遮罩关闭也天然落在同一个事件循环里。
//
// 遮罩色用 @overlay 令牌（浅色是近白半透明、深色是近黑半透明），正好是
// "与主表面相反"的语义，不需要新增颜色令牌。
//
// 同样不带 Q_OBJECT：按钮点击一律走 std::function，面板在外层决定行为。
// ---------------------------------------------------------------------------

#include "MemeSource.h"

#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QMovie;
class QPushButton;

namespace meme {

class MemePreview : public QWidget
{
public:
    explicit MemePreview(QWidget *parent = nullptr);

    /// 打开预览。path 为空时先显示 busyText（原图还在下载），拿到后再调 setImagePath。
    void openItem(const MemeItem &item, const QString &path, bool animated, const QString &meta,
                  const QString &busyText = QString());

    /// 图片换了（从缩略图换成原图，或刚下载完）。
    void setImagePath(const QString &path, bool animated, const QString &meta);
    void setMessage(const QString &text);

    void closePreview();
    bool isOpen() const { return isVisible(); }

    std::function<void()> onCopy;
    std::function<void()> onSaveAs;
    /// 关闭时通知面板：面板据此恢复网格里被暂停的动图播放。
    std::function<void()> onClosed;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QSize imageBoxSize() const;
    void layoutCard();
    void applyImage();
    void stopMovie();
    /// 标题是单行、不换行：按卡片宽度省略，超过就尾巴打点。
    void updateTitleElide();

    QWidget *m_card = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_image = nullptr;
    QLabel *m_meta = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_close = nullptr;

    QMovie *m_movie = nullptr;
    QString m_path;
    QString m_titleText;
    QString m_metaText;
    bool m_animated = false;
    /// 上一次按哪个图片框尺寸解的码。用来避免 resize 时无谓地重建播放器。
    QSize m_appliedBox;
};

} // namespace meme

#include "IconBadge.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>

namespace {

// 配色取自常见的浅色主题调色板：饱和度足够在浅底上辨识，又不会扎眼。
const QColor kPalette[] = {
    QColor(QStringLiteral("#4F6BF6")), QColor(QStringLiteral("#F59E0B")),
    QColor(QStringLiteral("#10B981")), QColor(QStringLiteral("#EF4444")),
    QColor(QStringLiteral("#8B5CF6")), QColor(QStringLiteral("#06B6D4")),
    QColor(QStringLiteral("#EC4899")), QColor(QStringLiteral("#6366F1")),
};

/// FNV-1a 哈希：分布均匀、实现只有两行，用来把 id 稳定地映射到调色板。
QColor colorFor(const QString &seed)
{
    uint h = 2166136261u;
    for (const QChar c : seed)
        h = (h ^ c.unicode()) * 16777619u;
    return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

/// 首字符；非字母（例如中文）原样返回，字母统一转大写。
QString initialOf(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return QStringLiteral("?");
    const QChar first = trimmed.at(0);
    return first.isLetter() ? QString(first).toUpper() : QString(first);
}

} // namespace

QPixmap IconBadge::pixmap(const QString &seed, const QString &name, int size, qreal devicePixelRatio)
{
    const qreal dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    const int px = qMax(1, qRound(size * dpr));

    QPixmap pm(px, px);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // 关键：setDevicePixelRatio 之后，画笔坐标是「逻辑像素」。
    // 这里必须用 size 而不是 px，否则 dpr > 1 时图形会画到画布外，右下角被裁掉。
    const QRectF r(0, 0, size, size);
    const qreal radius = size * 0.28;

    QLinearGradient grad(r.topLeft(), r.bottomRight());
    const QColor base = colorFor(seed.isEmpty() ? name : seed);
    grad.setColorAt(0.0, base.lighter(112));
    grad.setColorAt(1.0, base.darker(108));

    p.setPen(Qt::NoPen);
    p.setBrush(grad);
    p.drawRoundedRect(r, radius, radius);

    QFont f = p.font();
    f.setPixelSize(qMax(8, qRound(size * 0.46)));   // 逻辑像素字号，跟着 size 缩放
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 240));
    p.drawText(r, Qt::AlignCenter, initialOf(name));

    p.end();
    return pm;
}

QIcon IconBadge::icon(const QString &seed, const QString &name)
{
    // 每个逻辑尺寸都补 1x / 2x 两份。
    //
    // 只塞 2x 图会让 QIcon 把 96px 当成 48 逻辑尺寸，系统再按 24 显示时缩小，
    // 而缩小时它可能挑错源，表现为图标缺一角（踩过）。给全两档最稳。
    //
    // 96 是给首页磁贴留的：磁贴图标 48 逻辑像素，2x 屏需要 96 源图，
    // 少了这一档 QIcon 只能把 64 放大，边缘会糊。
    QIcon ic;
    const int sizes[] = {16, 24, 32, 48, 64, 96};
    for (int size : sizes) {
        ic.addPixmap(pixmap(seed, name, size, 1.0));
        ic.addPixmap(pixmap(seed, name, size, 2.0));
    }
    return ic;
}

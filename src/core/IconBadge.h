#pragma once

// ---------------------------------------------------------------------------
// 字母徽章图标生成器。
//
// 工具没有自带图标时，用「首字母 + 渐变色块」凑一个图标：色调由 id/name 做
// FNV 哈希决定，同一个工具每次启动颜色一致，不会跳色。
//
// 这里集中的是最容易出错的 DPI 处理，见 .cpp 里的注释。
// ---------------------------------------------------------------------------

#include <QIcon>
#include <QPixmap>
#include <QString>

class IconBadge
{
public:
    /// 生成单个尺寸的徽章。
    /// @param seed 决定颜色的种子（一般传工具 id），为空则用 name
    /// @param name 取首字母用于显示
    /// @param size 逻辑像素尺寸（不是物理像素）
    /// @param devicePixelRatio 屏幕 DPR，>1 时会生成更大的画布并按逻辑坐标绘制
    static QPixmap pixmap(const QString &seed, const QString &name, int size, qreal devicePixelRatio = 1.0);

    /// 生成多尺寸 QIcon（16/24/32/48/64，各带 1x、2x 两档），供侧边栏/托盘/窗口使用。
    static QIcon icon(const QString &seed, const QString &name);
};

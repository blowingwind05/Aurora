// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QRect>

inline QPoint trayMenuPosition(const QRect &workArea, const QRect &icon, const QSize &menu)
{
    const int minX = workArea.x();
    const int minY = workArea.y();
    const int maxX = qMax(minX, workArea.x() + workArea.width() - menu.width());
    const int maxY = qMax(minY, workArea.y() + workArea.height() - menu.height());
    const int alignedRight = icon.x() + icon.width() - menu.width();
    const int above = icon.y() - menu.height();
    const int below = icon.y() + icon.height();
    const int y = above >= minY ? above : below;
    return {qBound(minX, alignedRight, maxX), qBound(minY, y, maxY)};
}

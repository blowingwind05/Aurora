// SPDX-License-Identifier: GPL-3.0-only
#include "DesktopTray.h"
#include "TrayMenuPlacement.h"

#include <QApplication>
#include <QCursor>
#include <QIcon>
#include <QScreen>

#ifdef Q_OS_WIN
#include <QPainter>
#include <QProxyStyle>
#include <QSettings>
#include <QStyleOptionMenuItem>

#include <windows.h>

namespace {

constexpr int trayWidth = 160;
constexpr int trayInset = 5;
constexpr int shadowExtent = 8;
constexpr int trayRadius = 8;
constexpr int rowHeight = 31;
constexpr int iconSize = 16;

struct TrayColors
{
    QColor panel;
    QColor border;
    QColor divider;
    QColor text;
    QColor hover;
    QColor hoverText;
    bool shadow = true;
};

TrayColors trayColors(bool dark)
{
    if (dark)
        return {{0x29, 0x2D, 0x35}, {0x4B, 0x52, 0x60}, {0x43, 0x49, 0x55},
                {0xEA, 0xED, 0xF4}, {0x39, 0x40, 0x4C}, {0xEA, 0xED, 0xF4}};
    return {Qt::white, {0xDB, 0xE2, 0xED}, {0xE6, 0xEA, 0xF1},
            {0x24, 0x36, 0x50}, {0xED, 0xF1, 0xF7}, {0x24, 0x36, 0x50}};
}

QColor windowColor(int index)
{
    const auto color = GetSysColor(index);
    return {GetRValue(color), GetGValue(color), GetBValue(color)};
}

TrayColors windowsTrayColors()
{
    HIGHCONTRASTW contrast{sizeof contrast};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        const auto text = windowColor(COLOR_WINDOWTEXT);
        return {windowColor(COLOR_WINDOW), text, text, text, windowColor(COLOR_HIGHLIGHT),
                windowColor(COLOR_HIGHLIGHTTEXT), false};
    }

    const QSettings appearance(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return trayColors(appearance.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0);
}

class WindowsTrayMenuStyle final : public QProxyStyle
{
public:
    WindowsTrayMenuStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

    TrayColors colors = trayColors(false);

    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
    {
        switch (metric) {
        case PM_MenuHMargin:
        case PM_MenuVMargin:
            return shadowExtent + trayInset;
        case PM_MenuPanelWidth:
            return 0;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                           const QWidget *widget) const override
    {
        if (type == CT_Menu)
            return size;
        if (type == CT_MenuItem) {
            const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
            if (item)
                return {trayWidth - 2 * trayInset, item->menuItemType == QStyleOptionMenuItem::Separator ? 7 : rowHeight};
        }
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override
    {
        if (element != PE_PanelMenu) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }

        const QRectF panel = QRectF(option->rect).adjusted(shadowExtent, shadowExtent, -shadowExtent, -shadowExtent);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        if (colors.shadow) {
            const auto shadow = panel.translated(0, 2);
            for (int spread = 6; spread >= 1; --spread) {
                painter->setBrush(QColor(0, 0, 0, spread >= 4 ? 3 : 6));
                painter->drawRoundedRect(shadow.adjusted(-spread, -spread, spread, spread),
                                         trayRadius + spread, trayRadius + spread);
            }
        }
        painter->setBrush(colors.panel);
        painter->setPen(colors.border);
        painter->drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), trayRadius, trayRadius);
        painter->restore();
    }

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override
    {
        if (element == CE_MenuEmptyArea)
            return;
        const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        if (element != CE_MenuItem || !item) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }

        const auto row = item->rect;
        if (item->menuItemType == QStyleOptionMenuItem::Separator) {
            painter->fillRect(QRect(row.x() + 4, row.center().y(), row.width() - 8, 1), colors.divider);
            return;
        }

        const bool selected = item->state.testFlag(State_Selected);
        const auto ink = selected ? colors.hoverText : colors.text;
        painter->save();
        if (selected) {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors.hover);
            painter->drawRoundedRect(row, 5, 5);
        }

        const auto scale = painter->device()->devicePixelRatioF();
        QPixmap glyph(qRound(iconSize * scale), qRound(iconSize * scale));
        glyph.setDevicePixelRatio(scale);
        glyph.fill(Qt::transparent);
        {
            QPainter image(&glyph);
            item->icon.paint(&image, QRect(0, 0, iconSize, iconSize));
            image.setCompositionMode(QPainter::CompositionMode_SourceIn);
            image.fillRect(0, 0, iconSize, iconSize, ink);
        }
        painter->drawPixmap(row.x() + 8, row.y() + (row.height() - iconSize) / 2, glyph);
        auto font = item->font;
        font.setWeight(item->menuItemType == QStyleOptionMenuItem::DefaultItem ? QFont::DemiBold : QFont::Normal);
        painter->setFont(font);
        painter->setPen(ink);
        painter->drawText(row.adjusted(32, 0, -8, 0), Qt::AlignVCenter | Qt::TextSingleLine, item->text);
        painter->restore();
    }
};

void styleTrayMenu(QMenu &menu)
{
    menu.setWindowFlag(Qt::FramelessWindowHint);
    menu.setWindowFlag(Qt::NoDropShadowWindowHint);
    menu.setAttribute(Qt::WA_TranslucentBackground);
    auto font = menu.font();
    font.setPixelSize(13);
    menu.setFont(font);
    auto *style = new WindowsTrayMenuStyle;
    style->setParent(&menu);
    menu.setStyle(style);
    QObject::connect(&menu, &QMenu::aboutToShow, style, [style] { style->colors = windowsTrayColors(); });
}

} // namespace
#endif

bool DesktopTray::initialize()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return false;

#ifdef Q_OS_MACOS
    QIcon icon(QStringLiteral(":/aurora/brand/AuroraMenuBarTemplate.png"));
    icon.setIsMask(true);
#else
    const auto icon = QApplication::windowIcon();
#endif
    if (icon.isNull())
        return false;

    auto *open = menu.addAction(tr("打开 Aurora"), this, &DesktopTray::openRequested);
    auto *settings = menu.addAction(tr("设置"), this, &DesktopTray::settingsRequested);
    menu.addSeparator();
    auto *quit = menu.addAction(tr("退出 Aurora"), this, &DesktopTray::quitRequested);
#ifdef Q_OS_WIN
    open->setIcon(QIcon(QStringLiteral(":/aurora/tray/open.png")));
    settings->setIcon(QIcon(QStringLiteral(":/aurora/tray/settings.png")));
    quit->setIcon(QIcon(QStringLiteral(":/aurora/tray/dismiss.png")));
    menu.setDefaultAction(open);
    styleTrayMenu(menu);
#else
    Q_UNUSED(open);
    Q_UNUSED(settings);
    Q_UNUSED(quit);
#endif
    tray.setIcon(icon);
    tray.setToolTip(tr("Aurora 远程桌面"));
#ifdef Q_OS_WIN
    connect(&tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            emit openRequested();
        } else if (reason == QSystemTrayIcon::Context) {
            auto anchor = tray.geometry();
            if (!anchor.isValid())
                anchor = QRect(QCursor::pos(), QSize(1, 1));
            auto *screen = QGuiApplication::screenAt(anchor.center());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            if (screen) {
                menu.ensurePolished();
                menu.popup(trayMenuPosition(screen->availableGeometry(), anchor, menu.sizeHint()));
            }
        }
    });
#else
    tray.setContextMenu(&menu);
#endif
    return true;
}

bool DesktopTray::isAvailable() const
{
    return !tray.icon().isNull() && QSystemTrayIcon::isSystemTrayAvailable();
}

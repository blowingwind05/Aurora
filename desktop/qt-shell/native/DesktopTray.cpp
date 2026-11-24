// SPDX-License-Identifier: GPL-3.0-only
#include "DesktopTray.h"
#include "TrayMenuPlacement.h"

#include <QApplication>
#include <QCursor>
#include <QIcon>
#include <QScreen>

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

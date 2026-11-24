// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

class DesktopTray final : public QObject
{
    Q_OBJECT

public:
    bool initialize();
    bool isAvailable() const;
    bool isVisible() const { return tray.isVisible(); }
    void setVisible(bool visible) { tray.setVisible(visible); }

signals:
    void openRequested();
    void settingsRequested();
    void quitRequested();

private:
    QMenu menu;
    QSystemTrayIcon tray;
};

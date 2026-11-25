// SPDX-License-Identifier: GPL-3.0-only
#include "DesktopBridge.h"
#include "desktop/NativeDesktop.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>

DesktopBridge::DesktopBridge(NativeDesktop &desktop) : desktop_(desktop)
{
    connect(&desktop_, &NativeDesktop::stateChanged, this, &DesktopBridge::stateChanged);
}

DesktopBridge::~DesktopBridge() = default;

QString DesktopBridge::packagedEntryPoint()
{
    const QDir app(QCoreApplication::applicationDirPath());
    const QString entry = QStringLiteral("ui/desktop/app/qml/Main.qml");
#ifdef Q_OS_MACOS
    return app.absoluteFilePath(QStringLiteral("../Resources/") + entry);
#else
    const auto adjacent = app.absoluteFilePath(entry);
    if (QFileInfo::exists(adjacent))
        return adjacent;
    return app.absoluteFilePath(QStringLiteral("../../") + entry);
#endif
}

void DesktopBridge::installApplicationMenu()
{
#ifdef Q_OS_MACOS
    applicationMenu_ = std::make_unique<QMenuBar>();
    auto *application = applicationMenu_->addMenu(tr("Aurora"));
    auto *settings = application->addAction(tr("设置…"), this, [this] { desktop_.activate(QStringLiteral("settings")); });
    settings->setMenuRole(QAction::PreferencesRole);
    settings->setShortcut(QKeySequence::Preferences);
    auto *quit = application->addAction(tr("退出 Aurora"), &desktop_, &NativeDesktop::requestExit);
    quit->setMenuRole(QAction::QuitRole);
    quit->setShortcut(QKeySequence::Quit);
#endif
}

QVariantMap DesktopBridge::state() const
{
    return desktop_.state();
}

void DesktopBridge::send(const QString &action, const QString &deviceId, int deviceEpoch)
{
    desktop_.handleRequest({{"action", action}, {"deviceId", deviceId}, {"deviceEpoch", deviceEpoch}});
}

void DesktopBridge::sendPreference(const QString &action, int requestId, const QVariant &value)
{
    desktop_.handleRequest({{"action", action}, {"preferenceRequestId", requestId},
                            {"preferenceValue", QJsonValue::fromVariant(value)}});
}

void DesktopBridge::sendTailscale(const QString &action, const QString &target, int revision)
{
    desktop_.handleRequest({{"action", action}, {"accountTarget", target}, {"accountRevision", revision}});
}

void DesktopBridge::sendSunshine(const QString &action, const QVariantMap &data)
{
    desktop_.handleRequest({{"action", action}, {"sunshine", QJsonObject::fromVariantMap(data)}});
}

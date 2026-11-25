// SPDX-License-Identifier: GPL-3.0-only
#include "DesktopBridge.h"
#include "desktop/NativeDesktop.h"

#include <QJsonObject>

DesktopBridge::DesktopBridge(NativeDesktop &desktop) : desktop_(desktop)
{
    connect(&desktop_, &NativeDesktop::stateChanged, this, &DesktopBridge::stateChanged);
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

// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QVariantMap>

class NativeDesktop;

class DesktopBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit DesktopBridge(NativeDesktop &desktop);
    QVariantMap state() const;

    Q_INVOKABLE void send(const QString &action, const QString &deviceId = {}, int deviceEpoch = 0);
    Q_INVOKABLE void sendPreference(const QString &action, int requestId, const QVariant &value);
    Q_INVOKABLE void sendTailscale(const QString &action, const QString &target, int revision);
    Q_INVOKABLE void sendSunshine(const QString &action, const QVariantMap &data);

signals:
    void stateChanged();

private:
    NativeDesktop &desktop_;
};

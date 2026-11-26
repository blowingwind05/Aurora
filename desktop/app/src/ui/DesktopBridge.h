// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <memory>

class NativeDesktop;
class QMenuBar;

class DesktopBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit DesktopBridge(NativeDesktop &desktop);
    ~DesktopBridge() override;
    static QString packagedEntryPoint();
    void installApplicationMenu();
    QVariantMap state() const;

    Q_INVOKABLE void send(const QString &action, const QString &deviceId = {}, int deviceEpoch = 0);
    Q_INVOKABLE void sendPreference(const QString &action, int requestId, const QVariant &value);
    Q_INVOKABLE void sendTailscale(const QString &action, const QString &target, int revision);
    Q_INVOKABLE void sendSunshine(const QString &action, const QVariantMap &data);
    Q_INVOKABLE QUrl choosePrivacyAppearanceImage();

signals:
    void stateChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    NativeDesktop &desktop_;
    std::unique_ptr<QMenuBar> applicationMenu_;
};

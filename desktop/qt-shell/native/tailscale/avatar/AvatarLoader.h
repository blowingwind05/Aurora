// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Status.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;

namespace aurora::tailscale {

// Own on the coordinator's event-loop thread.
class AvatarLoader final : public QObject
{
    Q_OBJECT

public:
    explicit AvatarLoader(QObject *parent = nullptr);
    ~AvatarLoader() override;

    // Refresh this context when the provider, status or window visibility changes.
    // A hidden or ineligible context clears the published image and cancels its request.
    void setContext(quint64 generation, const Status &status, bool visible);
    QString dataUri() const { return data; }

signals:
    void dataUriChanged();

private:
    friend class AvatarLoaderTests;
    AvatarLoader(std::unique_ptr<QNetworkAccessManager> manager, QObject *parent);

    struct ImageKey
    {
        AccountIdentity account;
        QUrl url;

        bool operator==(const ImageKey &other) const { return account == other.account && url == other.url; }
    };
    struct Context
    {
        quint64 generation;
        ImageKey key;

        bool operator==(const Context &other) const { return generation == other.generation && key == other.key; }
    };
    struct Cached
    {
        ImageKey key;
        QString data;
        qint64 expires;
    };

    void start();
    void cancel();
    void finish(QString encoded = {});
    bool collect();
    void publish(QString encoded);

    const std::unique_ptr<QNetworkAccessManager> network;
    QNetworkReply *reply = nullptr;
    std::optional<Context> context;
    std::optional<Cached> cache;
    quint64 contextRevision = 0;
    QElapsedTimer clock;
    qint64 expires = 0;
    QTimer deadline;
    QByteArray body;
    QString data;
};

} // namespace aurora::tailscale

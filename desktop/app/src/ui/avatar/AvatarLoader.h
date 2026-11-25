// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Aurora::Ui {

// Own on the coordinator's event-loop thread.
class AvatarLoader final : public QObject
{
    Q_OBJECT

public:
    struct Source
    {
        QByteArray accountKey;
        QUrl url; // Canonical, allowlisted URL supplied by the caller.

        bool operator==(const Source &other) const { return accountKey == other.accountKey && url == other.url; }
    };

    explicit AvatarLoader(QObject *parent = nullptr);
    ~AvatarLoader() override;

    // A hidden or missing source clears the published image and cancels its request.
    void setContext(quint64 generation, const std::optional<Source> &source, bool visible);
    QString dataUri() const { return data; }

signals:
    void dataUriChanged();

private:
    friend class AvatarLoaderTests;
    AvatarLoader(std::unique_ptr<QNetworkAccessManager> manager, QObject *parent);

    struct Context
    {
        quint64 generation;
        Source key;

        bool operator==(const Context &other) const { return generation == other.generation && key == other.key; }
    };
    struct Cached
    {
        Source key;
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

} // namespace Aurora::Ui

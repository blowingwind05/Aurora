// SPDX-License-Identifier: GPL-3.0-only
#include "AvatarLoader.h"

#include <QBuffer>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPainter>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QVariant>

#include <utility>

namespace Aurora::Ui {
namespace {

constexpr int maxBodyBytes = 256 * 1024;
constexpr int maxHeaderBytes = 16 * 1024;
constexpr int maxImageDimension = 1024;
constexpr int avatarSize = 96;
constexpr int deadlineMs = 4000;
constexpr int successCacheMs = 600000;
constexpr int failureCacheMs = 30000;

bool validResponse(const QNetworkReply &reply)
{
    if (reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
        || reply.hasRawHeader("Content-Encoding")) {
        return false;
    }
    qint64 headerBytes = 16 + reply.attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray().size();
    for (const auto &field : reply.rawHeaderPairs()) {
        headerBytes += field.first.size() + field.second.size() + 4;
        if (headerBytes > maxHeaderBytes)
            return false;
    }

    if (reply.hasRawHeader("Content-Length")) {
        bool ok = false;
        const auto size = reply.rawHeader("Content-Length").toLongLong(&ok);
        if (!ok || size < 0 || size > maxBodyBytes)
            return false;
    }

    const auto mime = reply.rawHeader("Content-Type").split(';').first().trimmed().toLower();
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/webp";
}

bool validSize(const QSize &size)
{
    return !size.isEmpty() && size.width() <= maxImageDimension && size.height() <= maxImageDimension;
}

QString roundPng(const QByteArray &bytes)
{
    QBuffer input;
    input.setData(bytes);
    if (!input.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&input);
    reader.setDecideFormatFromContent(true);
    const auto format = reader.format().toLower();
    if ((format != "png" && format != "jpeg" && format != "jpg" && format != "webp") || !validSize(reader.size()))
        return {};
    const auto image = reader.read();
    if (!validSize(image.size()))
        return {};

    const qreal side = qMin(image.width(), image.height());
    const QRectF crop((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    const QRect rect(0, 0, avatarSize, avatarSize);
    QImage mask(rect.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(rect);
    }

    QImage output(rect.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    {
        QPainter painter(&output);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(rect, image, crop);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(0, 0, mask);
    }

    QBuffer png;
    png.open(QIODevice::WriteOnly);
    if (!output.save(&png, "PNG"))
        return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.data().toBase64());
}

} // namespace

AvatarLoader::AvatarLoader(QObject *parent)
    : AvatarLoader(std::make_unique<QNetworkAccessManager>(), parent)
{
}

AvatarLoader::AvatarLoader(std::unique_ptr<QNetworkAccessManager> manager, QObject *parent)
    : QObject(parent), network(std::move(manager))
{
    network->setProxy(QNetworkProxy::NoProxy);
    clock.start();
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    connect(&deadline, &QTimer::timeout, this, [this] { finish(); });
}

AvatarLoader::~AvatarLoader()
{
    cancel();
}

void AvatarLoader::setContext(quint64 generation, const std::optional<Source> &source, bool visible)
{
    std::optional<Context> next;
    if (visible && source && !source->accountKey.isEmpty() && source->url.isValid()
        && source->url.scheme() == QLatin1String("https") && source->url.port(443) == 443
        && !source->url.host().isEmpty() && !source->url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@'))
        && !source->url.hasFragment()) {
        next = Context{generation, *source};
    }

    const bool same = (!next && !context) || (next && context && *next == *context);
    if (same && (reply || !context || !data.isEmpty()))
        return;
    if (!same)
        cancel();
    context = std::move(next);
    const auto revision = ++contextRevision;
    const bool cached = context && cache && cache->key == context->key && clock.elapsed() < cache->expires;
    publish(cached ? cache->data : QString());
    if (!cached && context && contextRevision == revision)
        start();
}

void AvatarLoader::start()
{
    QNetworkRequest request(context->key.url);
    request.setRawHeader("Accept", "image/png, image/jpeg, image/webp");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);

    auto tls = QSslConfiguration::defaultConfiguration();
    tls.setLocalCertificateChain({});
    tls.setPrivateKey(QSslKey());
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    request.setSslConfiguration(tls);

    expires = clock.elapsed() + deadlineMs;
    deadline.start(deadlineMs);
    reply = network->get(request);
    reply->setReadBufferSize(maxBodyBytes + 1);

    const auto *active = reply;
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, active] {
        if (reply == active && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid() && !validResponse(*reply))
            finish();
    });
    connect(reply, &QIODevice::readyRead, this, [this, active] {
        if (reply == active && !collect())
            finish();
    });
    connect(reply, &QNetworkReply::finished, this, [this, active] {
        if (reply != active)
            return;
        if (reply->error() != QNetworkReply::NoError || !collect())
            return finish();
        if (reply->hasRawHeader("Content-Length") && reply->rawHeader("Content-Length").toLongLong() != body.size())
            return finish();
        finish(roundPng(body));
    });
    connect(reply, &QNetworkReply::sslErrors, this, [this, active] {
        if (reply == active)
            finish();
    });
}

bool AvatarLoader::collect()
{
    if (clock.elapsed() >= expires || !validResponse(*reply))
        return false;
    body += reply->read(maxBodyBytes + 1 - body.size());
    return body.size() <= maxBodyBytes;
}

void AvatarLoader::cancel()
{
    deadline.stop();
    body.clear();
    auto *old = std::exchange(reply, nullptr);
    if (!old)
        return;
    disconnect(old, nullptr, this, nullptr);
    if (old->isRunning())
        old->abort();
    old->deleteLater();
}

void AvatarLoader::finish(QString encoded)
{
    if (!reply)
        return;
    const auto now = clock.elapsed();
    if (now >= expires)
        encoded.clear();
    cancel();
    if (context)
        cache = Cached{context->key, encoded, now + (encoded.isEmpty() ? failureCacheMs : successCacheMs)};
    publish(std::move(encoded));
}

void AvatarLoader::publish(QString encoded)
{
    if (data == encoded)
        return;
    data = std::move(encoded);
    emit dataUriChanged();
}

} // namespace Aurora::Ui

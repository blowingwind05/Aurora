// SPDX-License-Identifier: GPL-3.0-only
#include "AvatarLoader.h"

#include <QBuffer>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTest>

#include <algorithm>
#include <cstring>

namespace Aurora::Ui {
namespace {

class MemoryReply final : public QNetworkReply
{
public:
    MemoryReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(ReadOnly | Unbuffered);
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override { return pending.size() + QNetworkReply::bytesAvailable(); }

    void headers(int code = 200, const QByteArray &mime = "image/png",
                 const QByteArray &name = {}, const QByteArray &value = {})
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, code);
        if (!mime.isEmpty())
            setRawHeader("Content-Type", mime);
        if (!name.isEmpty())
            setRawHeader(name, value);
        emit metaDataChanged();
    }

    void push(const QByteArray &bytes)
    {
        pending += bytes;
        emit readyRead();
    }

    void complete()
    {
        setFinished(true);
        emit finished();
    }

    void abort() override
    {
        aborted = true;
        setError(OperationCanceledError, QStringLiteral("fixture canceled"));
        complete();
    }

    bool aborted = false;

protected:
    qint64 readData(char *buffer, qint64 maxLength) override
    {
        const auto count = qMin(maxLength, qint64(pending.size()));
        if (!count)
            return isFinished() ? -1 : 0;
        std::memcpy(buffer, pending.constData(), size_t(count));
        pending.remove(0, count);
        return count;
    }

private:
    QByteArray pending;
};

class OfflineNetwork final : public QNetworkAccessManager
{
public:
    QList<QNetworkRequest> requests;
    QList<QPointer<MemoryReply>> replies;
    int abortedCount = 0;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoing) override
    {
        if (operation != GetOperation || outgoing)
            qFatal("The avatar fixture permits only GET without an upload.");
        requests.append(request);
        auto *reply = new MemoryReply(request, this);
        replies.append(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            if (reply->aborted)
                ++abortedCount;
        });
        return reply;
    }
};

AvatarLoader::Source account(qint64 owner = 41)
{
    return {QByteArray::number(owner), QUrl(QStringLiteral("https://avatars.githubusercontent.com/u/123?v=4"))};
}

QByteArray imageBytes(int width = 240, int height = 80, const char *format = "PNG")
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::red);
    if (width == 240 && height == 80) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (x < 80 || x >= 160)
                    image.setPixelColor(x, y, Qt::green);
            }
        }
    }
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, format))
        return {};
    return buffer.data();
}

void completeImage(MemoryReply &reply, const QByteArray &payload, const QByteArray &mime = "image/png")
{
    reply.headers(200, mime);
    reply.push(payload.left(16));
    reply.push(payload.mid(16));
    reply.complete();
}

QImage decodeDataUri(const QString &uri)
{
    const auto prefix = QStringLiteral("data:image/png;base64,");
    return uri.startsWith(prefix)
        ? QImage::fromData(QByteArray::fromBase64(uri.mid(prefix.size()).toLatin1()), "PNG") : QImage();
}

} // namespace

class AvatarLoaderTests final : public QObject
{
    Q_OBJECT

private slots:
    void requestPolicyAndRoundImage()
    {
        auto manager = std::make_unique<OfflineNetwork>();
        auto *mock = manager.get();
        mock->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("fixture.invalid"), 80));
        AvatarLoader loader(std::move(manager), nullptr);
        const auto status = account();
        QSignalSpy changes(&loader, &AvatarLoader::dataUriChanged);
        loader.setContext(19, status, false);
        QVERIFY(mock->requests.isEmpty());
        auto unvetted = status;
        unvetted.url = QUrl(QStringLiteral("http://avatars.githubusercontent.com/u/123"));
        loader.setContext(19, unvetted, true);
        QVERIFY(mock->requests.isEmpty());
        loader.setContext(19, status, true);
        loader.setContext(19, status, true);
        QCOMPARE(mock->requests.size(), 1);
        QCOMPARE(mock->proxy().type(), QNetworkProxy::NoProxy);

        const auto request = mock->requests.first();
        QCOMPARE(request.url(), status.url);
        QCOMPARE(request.rawHeader("Accept"), QByteArray("image/png, image/jpeg, image/webp"));
        QCOMPARE(request.rawHeader("Accept-Encoding"), QByteArray("identity"));
        QVERIFY(!request.hasRawHeader("Authorization"));
        QVERIFY(!request.hasRawHeader("Cookie"));
        QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
        QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
        QCOMPARE(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), int(QNetworkRequest::Manual));
        QCOMPARE(request.attribute(QNetworkRequest::AuthenticationReuseAttribute).toInt(), int(QNetworkRequest::Manual));
        QCOMPARE(request.sslConfiguration().peerVerifyMode(), QSslSocket::VerifyPeer);
        QVERIFY(request.sslConfiguration().localCertificateChain().isEmpty());
        QVERIFY(!request.attribute(QNetworkRequest::Http2AllowedAttribute, true).toBool());

        completeImage(*mock->replies.first(), imageBytes());
        QCOMPARE(changes.size(), 1);
        const auto image = decodeDataUri(loader.dataUri());
        QCOMPARE(image.size(), QSize(96, 96));
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(image.pixelColor(95, 95).alpha(), 0);
        QCOMPARE(image.pixelColor(48, 48), QColor(Qt::red));
        QCOMPARE(image.pixelColor(10, 48), QColor(Qt::red));

        const auto cached = loader.dataUri();
        loader.setContext(19, status, false);
        QVERIFY(loader.dataUri().isEmpty());
        loader.setContext(19, status, true);
        QCOMPARE(loader.dataUri(), cached);
        QCOMPARE(mock->requests.size(), 1);
    }

    void hidesAndRejectsStaleRequests()
    {
        auto manager = std::make_unique<OfflineNetwork>();
        auto *mock = manager.get();
        AvatarLoader loader(std::move(manager), nullptr);
        auto status = account();
        loader.setContext(20, status, true);
        const auto hidden = mock->replies.last();
        loader.setContext(20, status, false);
        QVERIFY(hidden->aborted);
        hidden->complete();
        QVERIFY(loader.dataUri().isEmpty());

        loader.setContext(21, status, true);
        const auto oldGeneration = mock->replies.last();
        loader.setContext(22, status, true);
        QVERIFY(oldGeneration->aborted);
        const auto oldOwner = mock->replies.last();
        status = account(42); // The same URL still requires a current identity.
        loader.setContext(22, status, true);
        QVERIFY(oldOwner->aborted);
        const auto oldUrl = mock->replies.last();
        status.url = QUrl(QStringLiteral("https://lh3.googleusercontent.com/fixture"));
        loader.setContext(22, status, true);
        QVERIFY(oldUrl->aborted);
        oldGeneration->complete();
        oldOwner->complete();
        oldUrl->complete();
        QVERIFY(loader.dataUri().isEmpty());

        completeImage(*mock->replies.last(), imageBytes());
        QVERIFY(!loader.dataUri().isEmpty());
        loader.setContext(23, std::nullopt, true);
        QVERIFY(loader.dataUri().isEmpty());
        status.accountKey.clear();
        loader.setContext(23, status, true);
        QCOMPARE(mock->requests.size(), 5);
    }

    void cacheIsAccountScoped()
    {
        auto manager = std::make_unique<OfflineNetwork>();
        auto *mock = manager.get();
        AvatarLoader loader(std::move(manager), nullptr);
        loader.setContext(20, account(41), true);
        completeImage(*mock->replies.last(), imageBytes());
        QVERIFY(!loader.dataUri().isEmpty());
        loader.setContext(21, account(42), true);
        QVERIFY(loader.dataUri().isEmpty());
        QCOMPARE(mock->requests.size(), 2);
    }

    void rejectsResponse_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *name : {"redirect", "auth", "encoding", "identity", "mime", "missing-mime",
                                "header-limit", "declared-limit", "truncated", "streaming-limit",
                                "large-image", "unsupported-format", "corrupt-image", "tls"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void rejectsResponse()
    {
        QFETCH(QString, scenario);
        auto manager = std::make_unique<OfflineNetwork>();
        auto *mock = manager.get();
        AvatarLoader loader(std::move(manager), nullptr);
        const auto status = account();
        loader.setContext(44, status, true);
        auto &reply = *mock->replies.first();
        if (scenario == "redirect")
            reply.headers(302, "image/png", "Location", "https://lh3.googleusercontent.com/fixture");
        else if (scenario == "auth")
            reply.headers(401, "image/png", "WWW-Authenticate", "Basic realm=fixture");
        else if (scenario == "encoding" || scenario == "identity")
            reply.headers(200, "image/png", "Content-Encoding", scenario == "encoding" ? "gzip" : "identity");
        else if (scenario == "mime")
            reply.headers(200, "application/octet-stream");
        else if (scenario == "missing-mime")
            reply.headers(200, {});
        else if (scenario == "header-limit")
            reply.headers(200, "image/png", "X-Fake-Large", QByteArray(16 * 1024, 'a'));
        else if (scenario == "declared-limit")
            reply.headers(200, "image/png", "Content-Length", "262145");
        else if (scenario == "truncated") {
            const auto payload = imageBytes();
            reply.headers(200, "image/png", "Content-Length", QByteArray::number(payload.size() + 10));
            reply.push(payload);
        } else if (scenario == "streaming-limit") {
            reply.headers();
            reply.push(QByteArray(128 * 1024, 'a'));
            reply.push(QByteArray(128 * 1024, 'b'));
            QVERIFY(!reply.aborted);
            reply.push("c");
        } else if (scenario == "large-image") {
            completeImage(reply, imageBytes(1025, 4));
        } else if (scenario == "unsupported-format") {
            const auto gif = QByteArray::fromHex("47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b");
            completeImage(reply, gif);
        } else if (scenario == "corrupt-image") {
            completeImage(reply, "not an image");
        } else {
            emit reply.sslErrors({QSslError(QSslError::SelfSignedCertificate)});
            QVERIFY(reply.aborted);
        }
        if (!reply.isFinished())
            reply.complete();
        QVERIFY(loader.dataUri().isEmpty());
        QVERIFY(loader.cache && loader.cache->data.isEmpty());
        loader.setContext(44, status, true);
        QCOMPARE(mock->requests.size(), 1);
    }

    void validFormats_data()
    {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<QByteArray>("mime");
        QTest::newRow("png") << QByteArray("PNG") << QByteArray("image/png; charset=binary");
        QTest::newRow("jpeg") << QByteArray("JPEG") << QByteArray("Image/JPEG");
        QTest::newRow("webp") << QByteArray("WEBP") << QByteArray("image/webp");
    }

    void validFormats()
    {
        QFETCH(QByteArray, format);
        QFETCH(QByteArray, mime);
        const auto bytes = imageBytes(96, 96, format.constData());
        if (bytes.isEmpty() && format == "WEBP")
            QSKIP("This Qt installation does not contain the WebP image plugin.");
        QVERIFY(!bytes.isEmpty());
        auto manager = std::make_unique<OfflineNetwork>();
        auto *mock = manager.get();
        AvatarLoader loader(std::move(manager), nullptr);
        loader.setContext(20, account(), true);
        completeImage(*mock->replies.last(), bytes, mime);
        QCOMPARE(decodeDataUri(loader.dataUri()).size(), QSize(96, 96));
    }

    void absoluteDeadlineAndNegativeCache()
    {
        auto manager = std::make_unique<OfflineNetwork>();
        auto *mock = manager.get();
        AvatarLoader loader(std::move(manager), nullptr);
        QElapsedTimer elapsed;
        elapsed.start();
        loader.setContext(12, account(), true);
        const auto reply = mock->replies.first();
        reply->headers();
        reply->push(imageBytes().left(20));
        QTRY_COMPARE_WITH_TIMEOUT(mock->abortedCount, 1, 4900);
        QVERIFY(elapsed.elapsed() >= 3950);
        loader.setContext(12, account(), true);
        QCOMPARE(mock->requests.size(), 1);
        QVERIFY(loader.dataUri().isEmpty());

        loader.cache->expires = 0; // Advance the fixture's negative cache without waiting 30 seconds.
        loader.setContext(12, account(), true);
        QCOMPARE(mock->requests.size(), 2);
        completeImage(*mock->replies.last(), imageBytes());
        QVERIFY(!loader.dataUri().isEmpty());
    }
};

} // namespace Aurora::Ui

QTEST_GUILESS_MAIN(Aurora::Ui::AvatarLoaderTests)
#include "AvatarLoaderTests.moc"

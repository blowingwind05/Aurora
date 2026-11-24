// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraMore.h"

#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTest>

#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr int responseLimit = 1024 * 1024;
const auto networkError = QStringLiteral("无法连接 GitHub，请检查网络后重试。");
const auto noStable = QStringLiteral("GitHub 未返回可用的正式版本。");
const auto latest = QStringLiteral("当前已是最新版本");

QByteArray release(const QByteArray &tag)
{
    return "{\"draft\":false,\"prerelease\":false,\"tag_name\":\"" + tag + "\"}";
}

class MemoryReply final : public QNetworkReply
{
public:
    explicit MemoryReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        open(ReadOnly | Unbuffered);
    }

    bool isSequential() const override { return true; }

    qint64 bytesAvailable() const override
    {
        return data.size() + QNetworkReply::bytesAvailable();
    }

    void push(const QByteArray &bytes)
    {
        data.append(bytes);
        emit readyRead();
    }

    void announceLength(qint64 bytes)
    {
        setHeader(QNetworkRequest::ContentLengthHeader, bytes);
        emit metaDataChanged();
    }

    void complete(int status, NetworkError error = NoError)
    {
        if (status)
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (error != NoError)
            setError(error, QStringLiteral("offline fixture"));
        setFinished(true);
        emit finished();
    }

    void abort() override
    {
        aborted = true;
        complete(0, OperationCanceledError);
    }

    bool aborted = false;

protected:
    qint64 readData(char *buffer, qint64 maxLength) override
    {
        const qint64 count = qMin(maxLength, qint64(data.size()));
        if (!count)
            return isFinished() ? -1 : 0;
        std::memcpy(buffer, data.constData(), count);
        data.remove(0, count);
        return count;
    }

private:
    QByteArray data;
};

class OfflineNetwork final : public QNetworkAccessManager
{
public:
    int requests = 0;
    MemoryReply *reply = nullptr;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
    {
        Q_ASSERT(operation == GetOperation);
        Q_UNUSED(operation);
        ++requests;
        reply = new MemoryReply(request, this);
        return reply;
    }
};

QList<QUrl> openedUrls;
bool openSucceeded = true;

bool fakeOpen(const QUrl &url)
{
    openedUrls.append(url);
    return openSucceeded;
}

class OfflineBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state CONSTANT)

public:
    QVariantMap state() const
    {
        return {{"deviceAccess", "signedOut"}, {"reduceMotion", true}, {"applicationVersion", "0.8.0.0"}};
    }

    Q_INVOKABLE void send(const QString &action, const QString &, int)
    {
        requests.append(action);
    }

    QStringList requests;
};

class MoreTests final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        openedUrls.clear();
        openSucceeded = true;
    }

    void fixedLinks()
    {
        OfflineNetwork network;
        AuroraMore more(network, fakeOpen);
        QSignalSpy notices(&more, &AuroraMore::notice);
        QCOMPARE(more.feedbackAddress(), QStringLiteral("blowingwind05@mail.ustc.edu.cn"));
        for (const auto *action : {"source", "help", "mail", "issues", "author", "license",
                                   "moonlight", "sunshine", "tailscale", "SOURCE", "source?url=a",
                                   "https://example.invalid/", "mailto:user@example.invalid", ""}) {
            more.request(QString::fromLatin1(action));
        }
        const QList<QUrl> expected{
            QUrl("https://github.com/blowingwind05/Aurora"),
            QUrl::fromEncoded("https://github.com/blowingwind05/Aurora"
                "#%E4%BB%8E%E6%9E%81%E5%85%89%E7%AA%97%E5%8F%A3%E5%BC%80%E5%A7%8B"),
            QUrl("mailto:blowingwind05@mail.ustc.edu.cn"),
            QUrl("https://github.com/blowingwind05/Aurora/issues/new/choose"),
            QUrl("https://github.com/blowingwind05"),
            QUrl("https://github.com/blowingwind05/Aurora/blob/main/LICENSE"),
            QUrl("https://github.com/moonlight-stream/moonlight-qt"),
            QUrl("https://github.com/SunshineStream/Sunshine"),
            QUrl("https://github.com/tailscale/tailscale")
        };
        QCOMPARE(openedUrls, expected);
        QVERIFY(notices.isEmpty());
        QCOMPARE(network.requests, 0);
        openSucceeded = false;
        more.request("source");
        more.request("mail");
        QCOMPARE(notices.size(), 2);
        QCOMPARE(notices.at(0).first().toString(), QStringLiteral("无法打开链接。"));
        QCOMPARE(notices.at(1).first().toString(), QStringLiteral("无法打开系统邮件应用。"));
    }

    void requestPolicyAndDeduplication()
    {
        OfflineNetwork network;
        AuroraMore more(network, fakeOpen);
        more.request("checkUpdates");
        QCOMPARE(network.requests, 0);
        more.setApplicationVersion("0.8.0.0");
        QCOMPARE(more.versionLabel(), QStringLiteral("版本 0.8.0"));

        QSignalSpy checking(&more, &AuroraMore::checkingChanged);
        QSignalSpy notices(&more, &AuroraMore::notice);
        more.request("checkUpdates");
        more.request("checkUpdates");
        QCOMPARE(network.requests, 1);
        QCOMPARE(checking.size(), 1);
        QVERIFY(more.checking());
        const auto request = network.reply->request();
        QCOMPARE(request.url(), QUrl("https://api.github.com/repos/blowingwind05/Aurora/releases/latest"));
        QCOMPARE(request.rawHeader("User-Agent"), QByteArray("Aurora/0.8.0.0"));
        QCOMPARE(request.rawHeader("Accept"), QByteArray("application/vnd.github+json"));
        QCOMPARE(request.rawHeader("X-GitHub-Api-Version"), QByteArray("2022-11-28"));
        QCOMPARE(request.rawHeader("Accept-Encoding"), QByteArray("identity"));
        QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
        QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), int(QNetworkRequest::Manual));
        QCOMPARE(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), int(QNetworkRequest::Manual));

        network.reply->push(release("0.8"));
        network.reply->complete(200);
        QVERIFY(!more.checking());
        QCOMPARE(checking.size(), 2);
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().first().toString(), latest);

        more.request("checkUpdates");
        QCOMPARE(network.requests, 2);
        network.reply->complete(0, QNetworkReply::HostNotFoundError);
        QVERIFY(!more.checking());
        QCOMPARE(notices.size(), 2);
        QCOMPARE(notices.last().first().toString(), networkError);
    }

    void releaseResults_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("expected");
        QTest::newRow("newer") << release(" V0.8.0.1 ") << 200 << QStringLiteral("发现新版本 0.8.0.1。");
        QTest::newRow("normalizes-two-parts") << release("v0.8") << 200 << latest;
        QTest::newRow("normalizes-zero-revision") << release("0.8.0.0") << 200 << latest;
        QTest::newRow("numeric-order") << release("0.10") << 200 << QStringLiteral("发现新版本 0.10.0。");
        QTest::newRow("current-ahead") << release("0.7.9.1") << 200 << QStringLiteral("当前版本高于已发布的正式版本 0.7.9.1。");
        QTest::newRow("version-int32-rules") << release("v +0 . 08. -0") << 200 << latest;
        QTest::newRow("int32-overflow") << release("2147483648.0") << 200 << QStringLiteral("无法识别 GitHub 的正式版本号。");
        QTest::newRow("negative") << release("0.-8") << 200 << QStringLiteral("无法识别 GitHub 的正式版本号。");
        QTest::newRow("single-part") << release("8") << 200 << QStringLiteral("无法识别 GitHub 的正式版本号。");
        QTest::newRow("extra-part") << release("0.8.0.0.0") << 200 << QStringLiteral("无法识别 GitHub 的正式版本号。");
        QTest::newRow("suffix") << release("0.8-rc.1") << 200 << QStringLiteral("无法识别 GitHub 的正式版本号。");
        QTest::newRow("draft") << QByteArray("{\"draft\":true,\"prerelease\":false,\"tag_name\":\"9.9\"}") << 200 << noStable;
        QTest::newRow("prerelease") << QByteArray("{\"draft\":false,\"prerelease\":true,\"tag_name\":\"9.9\"}") << 200 << noStable;
        QTest::newRow("missing-prerelease") << QByteArray("{\"draft\":false,\"tag_name\":\"9.9\"}") << 200 << noStable;
        QTest::newRow("explicit-bool") << QByteArray("{\"draft\":false,\"prerelease\":0,\"tag_name\":\"9.9\"}") << 200 << noStable;
        QTest::newRow("string-tag") << QByteArray("{\"draft\":false,\"prerelease\":false,\"tag_name\":8}") << 200 << noStable;
        QTest::newRow("primitive-root") << QByteArray("true") << 200 << noStable;
        QTest::newRow("invalid-json") << QByteArray("{") << 200 << QStringLiteral("无法读取 GitHub 的版本信息。");
        QTest::newRow("not-found") << QByteArray() << 404 << QStringLiteral("GitHub 未返回可用的正式版本（HTTP 404）。");
        QTest::newRow("rate-limited") << QByteArray() << 429 << QStringLiteral("无法检查更新。GitHub 返回 HTTP 429。");
        QTest::newRow("redirect") << release("99.9") << 302 << QStringLiteral("无法检查更新。GitHub 返回 HTTP 302。");
        QTest::newRow("2xx-other") << release("0.8") << 201 << latest;
    }

    void releaseResults()
    {
        QFETCH(QByteArray, body);
        QFETCH(int, status);
        QFETCH(QString, expected);
        OfflineNetwork network;
        AuroraMore more(network, fakeOpen);
        more.setApplicationVersion("0.8.0.0");
        QSignalSpy notices(&more, &AuroraMore::notice);
        more.request("checkUpdates");
        if (!body.isEmpty())
            network.reply->push(body);
        network.reply->complete(status, status == 404 ? QNetworkReply::ContentNotFoundError : QNetworkReply::NoError);
        QCOMPARE(network.requests, 1);
        QVERIFY(!more.checking());
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().first().toString(), expected);
    }

    void unknownVersion()
    {
        OfflineNetwork network;
        AuroraMore more(network, fakeOpen);
        more.setApplicationVersion({});
        QCOMPARE(more.versionLabel(), QStringLiteral("版本未知"));
        QSignalSpy notices(&more, &AuroraMore::notice);
        more.request("checkUpdates");
        QCOMPARE(network.requests, 0);
        QVERIFY(!more.checking());
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().first().toString(), QStringLiteral("无法读取当前版本。"));
    }

    void sizeLimit()
    {
        OfflineNetwork network;
        AuroraMore more(network, fakeOpen);
        more.setApplicationVersion("0.8.0.0");
        QSignalSpy notices(&more, &AuroraMore::notice);

        auto exact = release("0.8");
        exact.append(QByteArray(responseLimit - exact.size(), ' '));
        more.request("checkUpdates");
        network.reply->announceLength(responseLimit);
        network.reply->push(exact);
        network.reply->complete(200);
        QVERIFY(!network.reply->aborted);
        QCOMPARE(notices.last().first().toString(), latest);

        more.request("checkUpdates");
        network.reply->push(exact.left(responseLimit / 2));
        network.reply->push(exact.mid(responseLimit / 2));
        QVERIFY(more.checking());
        network.reply->push(" ");
        QVERIFY(network.reply->aborted);
        QVERIFY(!more.checking());
        QCOMPARE(notices.last().first().toString(), networkError);

        more.request("checkUpdates");
        network.reply->announceLength(responseLimit + 1);
        QVERIFY(network.reply->aborted);
        QVERIFY(!more.checking());
        QCOMPARE(notices.size(), 3);
        QCOMPARE(notices.last().first().toString(), networkError);
    }

    void deadlineCoversStreamingResponse()
    {
        OfflineNetwork network;
        AuroraMore more(network, fakeOpen);
        more.setApplicationVersion("0.8.0.0");
        QSignalSpy notices(&more, &AuroraMore::notice);
        QElapsedTimer elapsed;
        elapsed.start();
        more.request("checkUpdates");
        QTimer::singleShot(3000, network.reply, [&network] { network.reply->push("{"); });
        QTRY_COMPARE_WITH_TIMEOUT(notices.size(), 1, 9500);
        QVERIFY(elapsed.elapsed() >= 7900);
        QVERIFY(!more.checking());
        QCOMPARE(notices.first().first().toString(), QStringLiteral("检查更新超时，请重试。"));
        more.request("checkUpdates");
        QCOMPARE(network.requests, 2);
        network.reply->push(release("0.8"));
        network.reply->complete(200);
        QCOMPARE(notices.last().first().toString(), latest);
    }

    void liveWindow()
    {
        OfflineBridge bridge;
        OfflineNetwork network;
        AuroraMore more(network, fakeOpen);
        more.setApplicationVersion(bridge.state().value("applicationVersion").toString());
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("auroraBridge", &bridge);
        engine.rootContext()->setContextProperty("auroraMore", &more);
        engine.load(QUrl::fromLocalFile(QStringLiteral(MORE_LIVE_QML)));
        QCOMPARE(engine.rootObjects().size(), 1);
        auto *window = engine.rootObjects().first();
        QCOMPARE(window->property("moreVersion").toString(), QStringLiteral("版本 0.8.0"));
        QCOMPARE(window->property("moreFeedbackAddress").toString(), more.feedbackAddress());

        auto *button = window->findChild<QObject *>("navMore");
        auto *menu = window->findChild<QObject *>("sidebarMenu");
        auto *update = window->findChild<QObject *>("checkUpdates");
        auto *spinner = window->findChild<QObject *>("updateRing");
        auto *notice = window->findChild<QObject *>("moreNotice");
        auto *source = window->findChild<QObject *>("moreSourceMenu");
        QVERIFY(button && menu && update && spinner && notice && source);
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
        QTRY_VERIFY(menu->property("opened").toBool());
        QVERIFY(QMetaObject::invokeMethod(update, "clicked"));
        QVERIFY(QMetaObject::invokeMethod(update, "clicked"));
        QCOMPARE(network.requests, 1);
        QCOMPARE(update->property("text").toString(), QStringLiteral("正在检查…"));
        QVERIFY(!update->property("enabled").toBool());
        QVERIFY(spinner->property("visible").toBool());

        network.reply->push(release("v0.9.0"));
        network.reply->complete(200);
        QTRY_VERIFY(!menu->property("opened").toBool());
        QCOMPARE(update->property("text").toString(), QStringLiteral("检查更新"));
        QVERIFY(update->property("enabled").toBool());
        QVERIFY(!spinner->property("visible").toBool());
        QCOMPARE(window->property("moreNoticeText").toString(), QStringLiteral("发现新版本 0.9.0。"));
        QVERIFY(notice->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(source, "triggered"));
        QCOMPARE(openedUrls.size(), 1);
        QCOMPARE(openedUrls.first(), QUrl("https://github.com/blowingwind05/Aurora"));
        QVERIFY(bridge.requests.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!notice->property("visible").toBool(), 3800);
    }
};

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    qputenv("QML_DISABLE_DISK_CACHE", "1");
    QGuiApplication app(argc, argv);
    qInfo() << "Qt runtime:" << qVersion() << "TLS backend:" << QSslSocket::sslLibraryVersionString();
    MoreTests suite;
    return QTest::qExec(&suite, argc, argv);
}

#include "MoreTests.moc"

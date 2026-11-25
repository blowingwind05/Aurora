// SPDX-License-Identifier: GPL-3.0-only
#include "SessionSupervisor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace Aurora::Session;
using aurora::tailscale::EmbeddedStatusClient;
using aurora::tailscale::PresenceResult;
using aurora::tailscale::StatusError;

namespace {

constexpr quint64 generation = 18;
const QHostAddress peer(QStringLiteral("100.76.1.20"));
const QString hostId = QStringLiteral("f43a3fe0-27f6-46b3-8c38-758349e23db1");

SessionCandidate directCandidate()
{
    return {generation, hostId, "Synthetic session", DirectRoute{QHostAddress("192.0.2.17"), 48989}};
}

SessionCandidate embeddedCandidate()
{
    return {generation, hostId, "Synthetic relay", EmbeddedRoute{"peer", peer}};
}

struct Fixture
{
    QTemporaryDir temp;
    QString program;
    QString launched;
    QString privacyLog;
    QString exitPlayback;

    Fixture()
    {
        program = temp.filePath("AuroraStream");
#ifdef Q_OS_WIN
        program += ".exe";
#endif
        launched = temp.filePath("playback-started");
        privacyLog = temp.filePath("privacy");
        exitPlayback = temp.filePath("exit-playback");
    }

    bool install(const QByteArray &mode = {}, const QByteArray &relay = {})
    {
        qputenv("AURORA_TEST_STREAM_LAUNCHED", QFile::encodeName(launched));
        qputenv("AURORA_TEST_STREAM_MODE", mode);
        qputenv("AURORA_TEST_STREAM_RELAY", relay);
        qputenv("AURORA_TEST_STREAM_HOST", hostId.toLatin1());
        qputenv("AURORA_TEST_STREAM_ENDPOINT", relay.isEmpty() ? QByteArray("192.0.2.17:48989") : "127.0.0.1:" + relay);
        qputenv("AURORA_TEST_PRIVACY_LOG", QFile::encodeName(privacyLog));
        qputenv("AURORA_TEST_PLAYBACK_EXIT", QFile::encodeName(exitPlayback));
        qunsetenv("AURORA_TEST_PRIVACY_MODE");
        return temp.isValid() && QFile::copy(QStringLiteral(SESSION_FAKE_CHILD), program);
    }

#ifdef Q_OS_WIN
    QList<QByteArray> privacyRequests() const
    {
        QFile file(privacyLog);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const auto contents = file.readAll().trimmed();
        return contents.isEmpty() ? QList<QByteArray>() : contents.split('\n');
    }
#endif
};

PreparedSession preparedAt(const QSignalSpy &spy, int index = 0)
{
    return spy.at(index).at(0).value<PreparedSession>();
}

SessionResult outcome(const QSignalSpy &spy, int index = 0)
{
    return spy.at(index).at(2).value<SessionResult>();
}

QByteArray probe(char nonce)
{
    QByteArray packet(192, '\0');
    packet.replace(0, 17, "AURORA/PRESENCE/1");
    packet[17] = 1;
    packet[18] = nonce;
    return packet;
}

SessionView makeView(QWindow &owner)
{
    owner.resize(780, 480);
    owner.show();
    SessionView view;
    view.owner = &owner;
    view.viewport = QRect(0, 44, 780, 436);
    return view;
}

#ifdef Q_OS_WIN
SessionPrivacyRoute privacyRoute(const PreparedSession &prepared)
{
    using namespace Aurora::Discovery;
    const auto &route = prepared.route;
    const bool embedded = route.viaEmbeddedTailnet;
    const Endpoint worker{route.address, quint16(route.port)};
    const auto endpoint = embedded ? Endpoint{peer, BundledSunshinePort} : worker;
    StreamHost selected{endpoint, route.hostId, "Synthetic", embedded, false, false, true};
    return {selected, {ServerInfo::Status::Complete, selected},
        embedded ? std::optional<Endpoint>(worker) : std::nullopt};
}

bool privacyValue(const SessionSupervisor &session, bool enabled, bool writable = true)
{
    const auto setting = session.privacyScreen().toMap();
    return setting.contains("value") && setting.value("value").toBool() == enabled &&
        setting.value("writable").toBool() == writable;
}
#endif

} // namespace

class SupervisorTests final : public QObject
{
    Q_OBJECT

private slots:
    void authorizationAndView();
    void relayStaysThroughWorkerExit();
    void cancelSentOpenDrainsItsLease();
    void invalidatedLeaseCannotBeAuthorized();
#ifdef Q_OS_WIN
    void rejectedAttachmentDoesNotFallBack();
    void unresponsiveWorkerIsReaped();
    void privacyTracksWorkerState();
    void privacyRequiresThePreparedRoute();
    void privacyStopsWithPlayback_data();
    void privacyStopsWithPlayback();
#endif
};

void SupervisorTests::authorizationAndView()
{
    Fixture fixture;
    QVERIFY(fixture.install("fullscreen"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QVERIFY(!attempt.isNull());
    QVERIFY(session.prepare(directCandidate()).isNull());
    QVERIFY(!session.start({attempt, generation}));
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 5000);
    QCOMPARE(preparedAt(prepared).attempt, attempt);
    QCOMPARE(preparedAt(prepared).generation, generation);
    QCOMPARE(preparedAt(prepared).route.address, QHostAddress("192.0.2.17"));
    QCOMPARE(preparedAt(prepared).executable, fixture.program);
    QVERIFY(!QFile::exists(fixture.launched));
    QVERIFY(!session.start({QUuid::createUuid(), generation}));
    QVERIFY(!session.start({attempt, generation + 1}));
    QVERIFY(!session.cancel(QUuid::createUuid()));

#ifdef Q_OS_WIN
    const auto view = makeView(owner);
    owner.setWindowState(Qt::WindowMaximized);
    QSignalSpy sizes(&session, &SessionSupervisor::clientPixelSizeChanged);
    QVERIFY(session.start({attempt, generation}, view));
    QVERIFY(!session.start({attempt, generation}, view));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    QVERIFY(session.videoReady());
    QTRY_COMPARE_WITH_TIMEOUT(session.connectionMetrics().value("latency").toString(), QString("30 ms"), 2000);
    QCOMPARE(session.connectionMetrics().value("codec").toString(), QString("HEVC"));
    QVERIFY(session.networkPoor());
    QTRY_VERIFY_WITH_TIMEOUT(session.fullScreen(), 2000);
    session.setFullScreen(false);
    QTRY_COMPARE(owner.windowState(), Qt::WindowMaximized);
    session.setViewport(QRect(0, 44, 540, 310), false);
    QTRY_VERIFY(!sizes.isEmpty() && sizes.last().at(0).value<QSize>().width() == qRound(540 * owner.devicePixelRatio()));
    QVERIFY(!owner.close()); // handled as a graceful worker stop while the HWND is kept alive
#else
    QSignalSpy settings(&session, &SessionSupervisor::preferenceChangeRequested);
    QVERIFY(session.start({attempt, generation}));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Standalone, 3000);
    QVERIFY(!session.videoReady());
    QTRY_COMPARE(settings.size(), 1);
    QCOMPARE(settings.first().first().toString(), QString("resolution"));
    QCOMPARE(settings.first().at(1).toString(), QString("1920x1080"));
    session.requestStop();
#endif
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3500);
    QCOMPARE(finished.first().at(0).toUuid(), attempt);
    QCOMPARE(outcome(finished).reason, SessionEnd::Cancelled);
    QVERIFY(!outcome(finished).forced);
    QVERIFY(QFile::exists(fixture.launched));
    QCOMPARE(session.phase(), SessionSupervisor::Phase::Idle);
    QVERIFY(!session.start({attempt, generation}));
    const auto retry = session.prepare(directCandidate());
    QVERIFY(!retry.isNull() && retry != attempt);
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 2, 5000);
    QVERIFY(!session.start({attempt, generation}));
    QVERIFY(session.cancel(retry));
    QCOMPARE(finished.size(), 2);
}

void SupervisorTests::relayStaysThroughWorkerExit()
{
    Fixture fixture;
    QVERIFY(fixture.install("play", "55011"));
    EmbeddedStatusClient tailnet(QStringLiteral(SESSION_TAILNET_FIXTURE), fixture.temp.filePath("rpc-session"));
    QSignalSpy status(&tailnet, &EmbeddedStatusClient::statusFinished);
    QSignalSpy closed(&tailnet, &EmbeddedStatusClient::closeFinished);
    QVERIFY(tailnet.requestStatus(generation));
    QTRY_COMPARE_WITH_TIMEOUT(status.size(), 1, 2000);

    QWindow owner;
    SessionSupervisor session(fixture.program, &tailnet);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    bool closedBeforeExit = false;
    connect(&tailnet, &EmbeddedStatusClient::closeFinished, &session, [&] {
        closedBeforeExit = finished.isEmpty();
    });
    const auto attempt = session.prepare(embeddedCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 5000);
    const auto route = preparedAt(prepared).route;
    QCOMPARE(route.address, QHostAddress(QHostAddress::LocalHost));
    QCOMPARE(route.port, 55011);
    QCOMPARE(route.hostId, hostId);
    QVERIFY(route.viaEmbeddedTailnet);
    QVERIFY(!QFile::exists(fixture.launched));
    QVERIFY(closed.isEmpty());

#ifdef Q_OS_WIN
    QVERIFY(session.start({attempt, generation}, makeView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
#else
    QVERIFY(session.start({attempt, generation}));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Standalone, 3000);
#endif
    QVERIFY(closed.isEmpty());
    session.requestStop();
    QCOMPARE(session.phase(), SessionSupervisor::Phase::Stopping);
    QVERIFY(closed.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3500);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 2000);
    QCOMPARE(closed.first().at(2).value<StatusError>(), StatusError::None);
    QVERIFY(!closedBeforeExit);
    QCOMPARE(outcome(finished).reason, SessionEnd::Cancelled);
    QVERIFY(!outcome(finished).forced);
}

void SupervisorTests::cancelSentOpenDrainsItsLease()
{
    Fixture fixture;
    QVERIFY(fixture.install());
    EmbeddedStatusClient tailnet(QStringLiteral(SESSION_TAILNET_FIXTURE), fixture.temp.filePath("rpc-cancel"));
    QSignalSpy status(&tailnet, &EmbeddedStatusClient::statusFinished);
    QSignalSpy presence(&tailnet, &EmbeddedStatusClient::presenceFinished);
    QSignalSpy closed(&tailnet, &EmbeddedStatusClient::closeFinished);
    QVERIFY(tailnet.requestStatus(generation));
    QTRY_COMPARE_WITH_TIMEOUT(status.size(), 1, 2000);

    SessionSupervisor session(fixture.program, &tailnet);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    QVERIFY(tailnet.requestPresence(generation, "peer", peer, probe('a')));
    const auto attempt = session.prepare(embeddedCandidate());
    // The fixture sends the first presence reply only after it has read the open.
    QTRY_COMPARE_WITH_TIMEOUT(presence.size(), 1, 2000);
    QCOMPARE(presence.first().at(2).value<PresenceResult>().error, StatusError::None);
    QCOMPARE(session.phase(), SessionSupervisor::Phase::OpeningRelay);
    QVERIFY(session.cancel(attempt));
    QCOMPARE(session.phase(), SessionSupervisor::Phase::Stopping);
    QVERIFY(finished.isEmpty());
    QVERIFY(session.prepare(directCandidate()).isNull());
    QVERIFY(tailnet.requestPresence(generation, "peer", peer, probe('b')));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 2000);
    QCOMPARE(closed.first().at(2).value<StatusError>(), StatusError::None);
    QCOMPARE(outcome(finished).reason, SessionEnd::Cancelled);
    QVERIFY(!session.start({attempt, generation}));
    QVERIFY(prepared.isEmpty());
    QVERIFY(!QFile::exists(fixture.launched));
}

void SupervisorTests::invalidatedLeaseCannotBeAuthorized()
{
    Fixture fixture;
    QVERIFY(fixture.install());
    EmbeddedStatusClient tailnet(QStringLiteral(SESSION_TAILNET_FIXTURE), fixture.temp.filePath("rpc-close-fault"));
    QSignalSpy status(&tailnet, &EmbeddedStatusClient::statusFinished);
    QSignalSpy opened(&tailnet, &EmbeddedStatusClient::openFinished);
    QVERIFY(tailnet.requestStatus(generation));
    QTRY_COMPARE_WITH_TIMEOUT(status.size(), 1, 2000);
    QVERIFY(tailnet.requestOpen(generation, "peer", peer));
    QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 2000);
    auto other = opened.takeFirst().at(2).value<aurora::tailscale::OpenResult>().lease;
    QVERIFY(other && other->isValid());

    SessionSupervisor session(fixture.program, &tailnet);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(embeddedCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 5000);
    QVERIFY(other->close()); // the fake helper fails this close and invalidates both leases
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(outcome(finished).reason, SessionEnd::RelayLost);
    QVERIFY(!session.start({attempt, generation}));
    QVERIFY(!QFile::exists(fixture.launched));
}

#ifdef Q_OS_WIN
void SupervisorTests::rejectedAttachmentDoesNotFallBack()
{
    Fixture fixture;
    QVERIFY(fixture.install("attach-fail"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation}, makeView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(outcome(finished).reason, SessionEnd::AttachmentFailed);
    QCOMPARE(outcome(finished).exitCode.value_or(0), 74);
    QVERIFY(!session.videoReady());
}

void SupervisorTests::unresponsiveWorkerIsReaped()
{
    Fixture fixture;
    QVERIFY(fixture.install("ignore-stop"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation}, makeView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    session.requestStop();
    QCOMPARE(session.phase(), SessionSupervisor::Phase::Stopping);
    QVERIFY(finished.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 9000);
    QCOMPARE(outcome(finished).reason, SessionEnd::Cancelled);
    QVERIFY(outcome(finished).forced);
    QCOMPARE(session.phase(), SessionSupervisor::Phase::Idle);
}

void SupervisorTests::privacyTracksWorkerState()
{
    Fixture fixture;
    QVERIFY(fixture.install());
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation, privacyRoute(preparedAt(prepared))}, makeView(owner)));
    session.setPrivacyScreen(true);
    QVERIFY(fixture.privacyRequests().isEmpty());

    QTRY_VERIFY_WITH_TIMEOUT(privacyValue(session, false), 3000);
    QCOMPARE(fixture.privacyRequests().size(), 1);
    QVERIFY(fixture.privacyRequests().last().startsWith("status "));

    session.setPrivacyScreen(true);
    QVERIFY(privacyValue(session, false, false)); // Keep the last confirmed state while changing.
    session.setPrivacyScreen(true);
    session.refreshPrivacyScreen();
    QTRY_VERIFY_WITH_TIMEOUT(privacyValue(session, true), 3000);
    QVERIFY(fixture.privacyRequests().last().startsWith("on "));
    QCOMPARE(fixture.privacyRequests().size(), 2);

    session.setPrivacyScreen(false);
    QTRY_VERIFY_WITH_TIMEOUT(privacyValue(session, false), 3000);
    QVERIFY(fixture.privacyRequests().last().startsWith("off "));

    qputenv("AURORA_TEST_PRIVACY_MODE", "failed");
    session.setPrivacyScreen(true);
    QTRY_VERIFY_WITH_TIMEOUT(!session.privacyScreen().isValid(), 3000);
    QVERIFY(!session.privacyScreenMessage().isEmpty());
    session.setPrivacyScreen(false);
    QCOMPARE(fixture.privacyRequests().size(), 4);

    qputenv("AURORA_TEST_PRIVACY_MODE", "unavailable");
    QSignalSpy feedback(&session, &SessionSupervisor::privacyScreenChanged);
    session.refreshPrivacyScreen();
    QTRY_COMPARE_WITH_TIMEOUT(feedback.size(), 2, 3000);
    QVERIFY(!session.privacyScreen().isValid());
    session.setPrivacyScreen(true);
    QCOMPARE(fixture.privacyRequests().size(), 5);

    qunsetenv("AURORA_TEST_PRIVACY_MODE");
    session.refreshPrivacyScreen();
    QTRY_VERIFY_WITH_TIMEOUT(privacyValue(session, false), 3000);
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QVERIFY(!session.privacyScreen().isValid());
}

void SupervisorTests::privacyRequiresThePreparedRoute()
{
    Fixture fixture;
    QVERIFY(fixture.install());
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    auto wrong = privacyRoute(preparedAt(prepared));
    ++wrong.selected.endpoint.port;
    wrong.revalidated.current = wrong.selected; // Internally consistent, but for another worker endpoint.
    QVERIFY(session.start({attempt, generation, wrong}, makeView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    session.refreshPrivacyScreen();
    session.setPrivacyScreen(true);
    QVERIFY(!session.privacyScreen().isValid() && session.privacyScreenMessage().isEmpty());
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QVERIFY(fixture.privacyRequests().isEmpty());
}

void SupervisorTests::privacyStopsWithPlayback_data()
{
    QTest::addColumn<QString>("trigger");
    QTest::newRow("stop") << QStringLiteral("stop");
    QTest::newRow("relay-loss") << QStringLiteral("relay-loss");
    QTest::newRow("playback-exit") << QStringLiteral("playback-exit");
}

void SupervisorTests::privacyStopsWithPlayback()
{
    QFETCH(QString, trigger);
    const bool loseRelay = trigger == "relay-loss";
    const bool exitPlayback = trigger == "playback-exit";
    Fixture fixture;
    QVERIFY(fixture.install(exitPlayback ? "exit-on-fixture" : "play", loseRelay ? "55012" : "55011"));
    qputenv("AURORA_TEST_PRIVACY_MODE", "hold");
    EmbeddedStatusClient tailnet(QStringLiteral(SESSION_TAILNET_FIXTURE),
        fixture.temp.filePath(loseRelay ? "rpc-close-fault" : "rpc-session"));
    QSignalSpy status(&tailnet, &EmbeddedStatusClient::statusFinished);
    QSignalSpy closed(&tailnet, &EmbeddedStatusClient::closeFinished);
    QVERIFY(tailnet.requestStatus(generation));
    QTRY_COMPARE_WITH_TIMEOUT(status.size(), 1, 2000);

    std::shared_ptr<aurora::tailscale::StreamLease> other;
    if (loseRelay) {
        QSignalSpy opened(&tailnet, &EmbeddedStatusClient::openFinished);
        QVERIFY(tailnet.requestOpen(generation, "peer", peer));
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 2000);
        other = opened.first().at(2).value<aurora::tailscale::OpenResult>().lease;
        QVERIFY(other && other->isValid());
    }

    QWindow owner;
    SessionSupervisor session(fixture.program, &tailnet);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(embeddedCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation, privacyRoute(preparedAt(prepared))}, makeView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.privacyRequests().size(), 1, 3000);
    QCOMPARE(session.phase(), SessionSupervisor::Phase::Streaming);
    const auto request = fixture.privacyRequests().first().split(' ');
    QCOMPARE(request.size(), 2);
    QCOMPARE(request.first(), QByteArray("status"));
    const std::unique_ptr<void, decltype(&CloseHandle)> child(OpenProcess(SYNCHRONIZE, FALSE, request.last().toULong()),
                                                            &CloseHandle);
    QVERIFY(child && WaitForSingleObject(child.get(), 0) == WAIT_TIMEOUT);
    bool exitedAtFinish = false;
    connect(&session, &SessionSupervisor::finished, &session, [&] {
        exitedAtFinish = WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0;
    });

    if (loseRelay) {
        QVERIFY(other->close());
    } else if (exitPlayback) {
        QFile marker(fixture.exitPlayback);
        QVERIFY(marker.open(QIODevice::WriteOnly));
    } else {
        session.requestStop();
    }
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3500);
    const auto expected = loseRelay ? SessionEnd::RelayLost : exitPlayback ? SessionEnd::Exited : SessionEnd::Cancelled;
    QCOMPARE(outcome(finished).reason, expected);
    QVERIFY(exitedAtFinish && !session.privacyScreen().isValid());
    if (!loseRelay) {
        QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 2000);
        QCOMPARE(closed.first().at(2).value<StatusError>(), StatusError::None);
    }
}
#endif

int main(int argc, char **argv)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);
    SupervisorTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "SupervisorTests.moc"

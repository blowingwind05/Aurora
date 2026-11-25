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
    QString videoLog;
    QString videoReadyGate;

    Fixture()
    {
        program = temp.filePath("AuroraStream");
#ifdef Q_OS_WIN
        program += ".exe";
#endif
        launched = temp.filePath("playback-started");
        privacyLog = temp.filePath("privacy");
        exitPlayback = temp.filePath("exit-playback");
        videoLog = temp.filePath("video-log");
        videoReadyGate = temp.filePath("video-ready");
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
        qputenv("AURORA_TEST_VIDEO_LOG", QFile::encodeName(videoLog));
        qputenv("AURORA_TEST_VIDEO_READY", QFile::encodeName(videoReadyGate));
        qputenv("AURORA_TEST_VIDEO_LOCK", QFile::encodeName(temp.filePath("video-lock")));
        qunsetenv("AURORA_TEST_PRIVACY_MODE");
        qunsetenv("AURORA_TEST_BITRATE_PENDING");
        qunsetenv("AURORA_TEST_PINNED_CONTROL");
        qunsetenv("AURORA_TEST_STREAM_PIN");
        return temp.isValid() && QFile::copy(QStringLiteral(SESSION_FAKE_CHILD), program);
    }

#ifdef Q_OS_WIN
    QList<QByteArray> privacyRequests() const { return records(privacyLog); }
    QList<QByteArray> videoEvents() const { return records(videoLog); }

    static QList<QByteArray> records(const QString &path)
    {
        QFile file(path);
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
SessionPlaybackSettings picture(QString fps, bool quit = false)
{
    return {[fps, quit](Aurora::Stream::PreferenceDefaults) {
                QStringList arguments{"--fps", fps};
                if (quit)
                    arguments.append("--quit-after");
                return arguments;
            }, QJsonObject{{"fps", fps}}};
}

SessionView pictureView(QWindow &owner, bool quit = false)
{
    auto view = makeView(owner);
    const auto settings = picture("30", quit);
    view.preferenceArguments = settings.preferenceArguments;
    view.sessionPreferences = settings.sessionPreferences;
    return view;
}

SessionControlRoute privacyRoute(const PreparedSession &prepared)
{
    using namespace Aurora::Discovery;
    const auto &route = prepared.route;
    const bool embedded = route.viaEmbeddedTailnet;
    const Endpoint worker{route.address, quint16(route.port)};
    const auto endpoint = embedded ? Endpoint{peer, BundledSunshinePort} : worker;
    StreamHost selected{endpoint, route.hostId, "Synthetic", embedded, false, false, true};
    return {selected, ServerInfo::CheckResult{ServerInfo::Status::Complete, selected},
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
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    void routeHandoff_data();
    void routeHandoff();
#endif
#ifdef Q_OS_WIN
    void weakRouteObservationResets();
    void bitrateChangesWithoutRestarting_data();
    void bitrateChangesWithoutRestarting();
    void videoRestartsInSameWindowAndLease();
    void videoRestoresPreviousSettings();
    void videoRespectsSafetyGuards_data();
    void videoRespectsSafetyGuards();
    void videoCancelDuringTeardown();
    void videoForcedOldWorkerCannotRestart();
    void videoLeaseRevokedBeforeReadiness();
    void rejectedAttachmentDoesNotFallBack();
    void transportLossCanResume_data();
    void transportLossCanResume();
    void unresponsiveWorkerIsReaped();
    void privacyTracksWorkerState();
    void privacyRequiresThePreparedRoute();
    void publicIpv6SessionControls_data();
    void publicIpv6SessionControls();
    void privacyStopsWithPlayback_data();
    void privacyStopsWithPlayback();
#endif
};

#ifdef Q_OS_WIN
void SupervisorTests::weakRouteObservationResets()
{
    Fixture fixture;
    QVERIFY(fixture.install("quality-reset"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation}, makeView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(session.networkPoorDurationMs() >= 120, 2000);

    QFile sampleGate(fixture.exitPlayback);
    QVERIFY(sampleGate.open(QIODevice::WriteOnly));
    sampleGate.close();
    QTRY_VERIFY_WITH_TIMEOUT(!session.networkPoor(), 2000);
    QCOMPARE(session.networkPoorDurationMs(), qint64(0));

    QElapsedTimer newWeakWindow;
    newWeakWindow.start();
    QVERIFY(sampleGate.remove());
    QTRY_VERIFY_WITH_TIMEOUT(session.networkPoorDurationMs() > 0, 2000);
    QVERIFY(session.networkPoorDurationMs() <= newWeakWindow.elapsed());

    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(session.networkPoorDurationMs(), qint64(0));
}
#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
void SupervisorTests::routeHandoff_data()
{
    QTest::addColumn<QByteArray>("mode");
    QTest::addColumn<bool>("pinned");
    QTest::newRow("clean") << QByteArray("route-switch-clean") << true;
    QTest::newRow("unbound") << QByteArray("route-switch-clean") << false;
#ifdef Q_OS_WIN
    QTest::newRow("explicit-cancel") << QByteArray("route-switch-cancel") << true;
    QTest::newRow("forced") << QByteArray("ignore-stop") << true;
#else
    QTest::newRow("privacy-veto") << QByteArray("route-switch-deny") << true;
    QTest::newRow("legacy") << QByteArray("route-switch-legacy") << true;
    QTest::newRow("unsolicited") << QByteArray("route-switch-unsolicited") << true;
#endif
}

void SupervisorTests::routeHandoff()
{
    QFETCH(QByteArray, mode);
    QFETCH(bool, pinned);
    Fixture fixture;
    QVERIFY(fixture.install(mode));
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    QSignalSpy denied(&session, &SessionSupervisor::routeSwitchDenied);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    SessionView view;
#ifdef Q_OS_WIN
    QWindow owner;
    view = makeView(owner);
#endif
    QVERIFY(session.start({attempt, generation, {}, pinned ? QByteArray(32, 'f') : QByteArray{}}, view));
    if (mode == "route-switch-unsolicited") {
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QCOMPARE(outcome(finished).reason, SessionEnd::Exited);
        return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);

    QVERIFY(!session.requestRouteSwitch(QUuid::createUuid()));
    if (!pinned || mode == "route-switch-legacy") {
        QVERIFY(!session.requestRouteSwitch(attempt));
        QCOMPARE(session.phase(), SessionSupervisor::Phase::Streaming);
        session.requestStop();
    } else {
        QVERIFY(session.requestRouteSwitch(attempt));
        QVERIFY(!session.requestRouteSwitch(attempt));
        if (mode == "route-switch-deny") {
            QTRY_COMPARE_WITH_TIMEOUT(denied.size(), 1, 3000);
            QCOMPARE(denied.at(0).at(0).toUuid(), attempt);
            QCOMPARE(session.phase(), SessionSupervisor::Phase::Streaming);
            QVERIFY(session.canChangeRoute());
            session.requestStop();
        } else if (mode == "route-switch-cancel") {
            session.requestStop();
        }
    }
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 7500);
    const auto expected = mode == "ignore-stop" ? SessionEnd::WorkerStartFailed
        : pinned && mode == "route-switch-clean" ? SessionEnd::RouteSwitched
        : SessionEnd::Cancelled;
    QCOMPARE(outcome(finished).reason, expected);
    QCOMPARE(outcome(finished).forced, mode == "ignore-stop");
}
#endif

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
void SupervisorTests::bitrateChangesWithoutRestarting_data()
{
    QTest::addColumn<bool>("pendingResult");
    QTest::newRow("confirmed") << false;
    QTest::newRow("late-confirmation") << true;
}

void SupervisorTests::bitrateChangesWithoutRestarting()
{
    QFETCH(bool, pendingResult);
    Fixture fixture;
    QVERIFY(fixture.install("video"));
    if (pendingResult)
        qputenv("AURORA_TEST_BITRATE_PENDING", "1");
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy applied(&session, &SessionSupervisor::videoSettingsApplied);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 5000);
    auto view = pictureView(owner, true);
    view.sessionPreferences.insert("bitrate", "20000");
    QVERIFY(session.start({attempt, generation, privacyRoute(preparedAt(prepared))}, std::move(view)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    const auto window = owner.winId();

    auto replacement = picture("30", true);
    replacement.sessionPreferences.insert("bitrate", "15000");
    QVERIFY(!session.reconfigureVideo(replacement));
    QVERIFY(session.videoReady());
    QTRY_COMPARE_WITH_TIMEOUT(session.videoSettings().value("liveBitrateMaximum").toInt(), 25000, 4000);
    const bool accepted = session.reconfigureVideo(replacement);
    QVERIFY2(accepted, qPrintable(session.videoSettingsMessage()));
    QVERIFY(session.videoReady());
    QTRY_COMPARE_WITH_TIMEOUT(applied.size(), 1, 4000);
    QCOMPARE(applied.first().first().toBool(), !pendingResult);
    QTRY_COMPARE_WITH_TIMEOUT(session.videoSettings().value("bitrate").toString(), QString("15000"), 4000);
    QCOMPARE(applied.size(), 1);
    QCOMPARE(fixture.videoEvents(), QList<QByteArray>({"start 30", "ready 30"}));
    QCOMPARE(owner.winId(), window);
    QVERIFY(session.videoReady());
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
}

void SupervisorTests::videoRestartsInSameWindowAndLease()
{
    Fixture fixture;
    QVERIFY(fixture.install("video", "55011"));
    EmbeddedStatusClient tailnet(QStringLiteral(SESSION_TAILNET_FIXTURE), fixture.temp.filePath("rpc-session"));
    QSignalSpy status(&tailnet, &EmbeddedStatusClient::statusFinished);
    QSignalSpy opened(&tailnet, &EmbeddedStatusClient::openFinished);
    QSignalSpy closed(&tailnet, &EmbeddedStatusClient::closeFinished);
    QVERIFY(tailnet.requestStatus(generation));
    QTRY_COMPARE_WITH_TIMEOUT(status.size(), 1, 2000);

    QWindow owner;
    SessionSupervisor session(fixture.program, &tailnet);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    QSignalSpy applied(&session, &SessionSupervisor::videoSettingsApplied);
    QSignalSpy requested(&session, &SessionSupervisor::videoSettingRequested);
    session.requestVideoSetting("fps", "50");
    QVERIFY(requested.isEmpty());
    const auto attempt = session.prepare(embeddedCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 5000);
    QVERIFY(session.start({attempt, generation}, pictureView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    const auto window = owner.winId();
    session.setFullScreen(true);
    QTRY_VERIFY(session.fullScreen());
    QCOMPARE(session.videoSettings().value("fps").toString(), QString("30"));
    session.requestVideoSetting("fps", "50");
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first(), QVariantList({QString("fps"), QString("50")}));

    QVERIFY(session.reconfigureVideo(picture("50")));
    QVERIFY(session.videoSettingsPending());
    QVERIFY(!session.videoReady());
    QVERIFY(!session.reconfigureVideo(picture("60")));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.videoEvents().contains("start 50"), 3000);
    QCOMPARE(session.videoSettings().value("fps").toString(), QString("30"));
    QVERIFY(applied.isEmpty());
    QCOMPARE(fixture.videoEvents(), QList<QByteArray>({"start 30", "ready 30", "exit 30", "start 50"}));
    QFile releaseReady(fixture.videoReadyGate);
    QVERIFY(releaseReady.open(QIODevice::WriteOnly));
    releaseReady.close();
    QTRY_COMPARE_WITH_TIMEOUT(applied.size(), 1, 3000);
    QCOMPARE(applied.first().first().toBool(), true);
    QCOMPARE(session.videoSettings().value("fps").toString(), QString("50"));
    QVERIFY(session.videoReady());
    QVERIFY(!session.videoSettingsPending());
    QCOMPARE(owner.winId(), window);
    QVERIFY(session.fullScreen());
    QCOMPARE(opened.size(), 1);
    QVERIFY(closed.isEmpty());
    QVERIFY(finished.isEmpty());
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 2000);
}

void SupervisorTests::videoRestoresPreviousSettings()
{
    Fixture fixture;
    QVERIFY(fixture.install("video"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy applied(&session, &SessionSupervisor::videoSettingsApplied);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation}, pictureView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    const auto window = owner.winId();
    QVERIFY(session.reconfigureVideo(picture("55")));
    QTRY_COMPARE_WITH_TIMEOUT(applied.size(), 1, 4000);
    QCOMPARE(applied.first().first().toBool(), false);
    QCOMPARE(fixture.videoEvents(), QList<QByteArray>({"start 30", "ready 30", "exit 30",
        "start 55", "failed 55", "start 30", "ready 30"}));
    QCOMPARE(session.videoSettings().value("fps").toString(), QString("30"));
    QVERIFY(session.videoReady());
    QVERIFY(!session.videoSettingsPending());
    QVERIFY(!session.videoSettingsMessage().isEmpty());
    QCOMPARE(owner.winId(), window);
    QVERIFY(finished.isEmpty());
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
}

void SupervisorTests::videoRespectsSafetyGuards_data()
{
    QTest::addColumn<QString>("guard");
    QTest::newRow("privacy-on") << QString("privacy");
    QTest::newRow("quit-current") << QString("quit-current");
    QTest::newRow("quit-replacement") << QString("quit-replacement");
}

void SupervisorTests::videoRespectsSafetyGuards()
{
    QFETCH(QString, guard);
    Fixture fixture;
    QVERIFY(fixture.install("video"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy applied(&session, &SessionSupervisor::videoSettingsApplied);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    const auto privacy = guard == "privacy"
        ? std::optional<SessionControlRoute>(privacyRoute(preparedAt(prepared))) : std::nullopt;
    QVERIFY(session.start({attempt, generation, privacy}, pictureView(owner, guard == "quit-current")));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    if (privacy) {
        QTRY_VERIFY_WITH_TIMEOUT(privacyValue(session, false), 3000);
        session.setPrivacyScreen(true);
        QTRY_VERIFY_WITH_TIMEOUT(privacyValue(session, true), 3000);
    }
    QCOMPARE(session.canChangeRoute(), guard == "quit-replacement");
    QVERIFY(!session.reconfigureVideo(picture("50", guard == "quit-replacement")));
    QVERIFY(!session.videoSettingsPending());
    QVERIFY(!session.videoSettingsMessage().isEmpty());
    QVERIFY(applied.isEmpty());
    QCOMPARE(fixture.videoEvents(), QList<QByteArray>({"start 30", "ready 30"}));
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
}

void SupervisorTests::videoCancelDuringTeardown()
{
    Fixture fixture;
    QVERIFY(fixture.install("video"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    QSignalSpy applied(&session, &SessionSupervisor::videoSettingsApplied);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation}, pictureView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    QVERIFY(session.reconfigureVideo(picture("50")));
    QVERIFY(session.cancel(attempt));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(outcome(finished).reason, SessionEnd::Cancelled);
    QCOMPARE(applied.size(), 1);
    QCOMPARE(applied.first().first().toBool(), false);
    QCOMPARE(fixture.videoEvents(), QList<QByteArray>({"start 30", "ready 30", "exit 30"}));
    QVERIFY(!session.start({attempt, generation}));
}

void SupervisorTests::videoForcedOldWorkerCannotRestart()
{
    Fixture fixture;
    QVERIFY(fixture.install("video-ignore-stop"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    QSignalSpy applied(&session, &SessionSupervisor::videoSettingsApplied);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation}, pictureView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    QVERIFY(session.reconfigureVideo(picture("50")));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 9000);
    QCOMPARE(outcome(finished).reason, SessionEnd::WorkerStartFailed);
    QVERIFY(outcome(finished).forced);
    QCOMPARE(applied.size(), 1);
    QCOMPARE(applied.first().first().toBool(), false);
    QCOMPARE(fixture.videoEvents(), QList<QByteArray>({"start 30", "ready 30"}));
}

void SupervisorTests::videoLeaseRevokedBeforeReadiness()
{
    Fixture fixture;
    // The first open is the other lease used to trigger the fixture fault.
    QVERIFY(fixture.install("video", "55012"));
    EmbeddedStatusClient tailnet(QStringLiteral(SESSION_TAILNET_FIXTURE), fixture.temp.filePath("rpc-close-fault"));
    QSignalSpy status(&tailnet, &EmbeddedStatusClient::statusFinished);
    QSignalSpy opened(&tailnet, &EmbeddedStatusClient::openFinished);
    QVERIFY(tailnet.requestStatus(generation));
    QTRY_COMPARE_WITH_TIMEOUT(status.size(), 1, 2000);
    QVERIFY(tailnet.requestOpen(generation, "peer", peer));
    QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 2000);
    auto other = opened.first().at(2).value<aurora::tailscale::OpenResult>().lease;
    QVERIFY(other && other->isValid());

    QWindow owner;
    SessionSupervisor session(fixture.program, &tailnet);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy applied(&session, &SessionSupervisor::videoSettingsApplied);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(embeddedCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(session.start({attempt, generation}, pictureView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    QVERIFY(session.reconfigureVideo(picture("50")));
    QTRY_VERIFY_WITH_TIMEOUT(fixture.videoEvents().contains("start 50"), 3000);
    QVERIFY(other->close());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 4000);
    QCOMPARE(outcome(finished).reason, SessionEnd::RelayLost);
    QCOMPARE(applied.size(), 1);
    QCOMPARE(applied.first().first().toBool(), false);
    QCOMPARE(fixture.videoEvents().count("start 30"), 1);
}

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
    session.setPrivacyMode("display-off");
    QTRY_COMPARE_WITH_TIMEOUT(session.privacyScreen().toMap().value("mode").toString(), QString("display-off"), 3000);
    QVERIFY(!session.canChangeRoute());
    session.setPrivacyMode("off");
    QTRY_COMPARE_WITH_TIMEOUT(session.privacyScreen().toMap().value("mode").toString(), QString("off"), 3000);
    qputenv("AURORA_TEST_PRIVACY_MODE", "no-virtual-display");
    session.setPrivacyMode("display-off");
    QTRY_VERIFY_WITH_TIMEOUT(privacyValue(session, false), 3000);
    QCOMPARE(session.privacyScreen().toMap().value("mode").toString(), QString("off"));
    QVERIFY(!session.privacyScreenMessage().isEmpty());
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
    wrong.revalidated->current = wrong.selected; // Internally consistent, but for another worker endpoint.
    QVERIFY(session.start({attempt, generation, wrong}, makeView(owner)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    session.refreshPrivacyScreen();
    session.setPrivacyScreen(true);
    QVERIFY(!session.privacyScreen().isValid() && session.privacyScreenMessage().isEmpty());
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QVERIFY(fixture.privacyRequests().isEmpty());
}

void SupervisorTests::publicIpv6SessionControls_data()
{
    QTest::addColumn<QByteArray>("scenario");
    QTest::newRow("authenticated-playback") << QByteArray("off");
    QTest::newRow("privacy-endpoint-absent") << QByteArray("unsupported");
    QTest::newRow("privacy-state-unconfirmed") << QByteArray("unavailable");
    QTest::newRow("old-worker") << QByteArray("legacy");
    QTest::newRow("pin-missing") << QByteArray("no-pin");
    QTest::newRow("endpoint-mismatch") << QByteArray("wrong-port");
}

void SupervisorTests::publicIpv6SessionControls()
{
    using namespace Aurora::Discovery;
    QFETCH(QByteArray, scenario);
    Fixture fixture;
    QVERIFY(fixture.install("video"));
    qputenv("AURORA_TEST_STREAM_ENDPOINT", "[2001:db8::17]:48989");
    const QByteArray pin = scenario == "no-pin" ? QByteArray{} : QByteArray(32, 'f');
    qputenv("AURORA_TEST_STREAM_PIN", pin.toHex());
    if (scenario != "legacy")
        qputenv("AURORA_TEST_PINNED_CONTROL", "1");
    qputenv("AURORA_TEST_PRIVACY_MODE", scenario);

    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    QSignalSpy privacyChanged(&session, &SessionSupervisor::privacyScreenChanged);
    auto candidate = directCandidate();
    const QHostAddress ipv6(QStringLiteral("2001:db8::17"));
    candidate.route = DirectRoute{ipv6, BundledSunshinePort};
    const auto attempt = session.prepare(candidate);
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    StreamHost selected{{ipv6, BundledSunshinePort}, hostId, "Synthetic"};
    selected.publicIpv6 = true;
    if (scenario == "wrong-port")
        ++selected.endpoint.port;
    auto view = pictureView(owner);
    const auto settings = picture("50");
    view.preferenceArguments = settings.preferenceArguments;
    view.sessionPreferences = settings.sessionPreferences;
    QVERIFY(session.start({attempt, generation, SessionControlRoute{selected, {}, {}}, pin}, std::move(view)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Connecting, 3000);
    session.refreshPrivacyScreen();
    session.refreshVideoBitrate();
    QVERIFY(fixture.privacyRequests().isEmpty());
    QVERIFY(session.videoSettings().value("bitrate").isNull());

    QFile ready(fixture.videoReadyGate);
    QVERIFY(ready.open(QIODevice::WriteOnly));
    ready.close();
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);
    const bool controls = scenario == "off" || scenario == "unsupported" || scenario == "unavailable";
    session.refreshVideoBitrate();
    if (controls) {
        QTRY_VERIFY_WITH_TIMEOUT(privacyChanged.size() >= 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(session.videoSettings().value("bitrate").toString(), QStringLiteral("20000"), 3000);
        QCOMPARE(session.privacyScreen().isValid(), scenario == "off");
        QCOMPARE(session.canChangeRoute(), scenario != "unavailable");
    } else {
        QVERIFY(!session.privacyScreen().isValid());
        QVERIFY(session.videoSettings().value("bitrate").isNull());
        QVERIFY(session.canChangeRoute());
    }
    session.requestStop();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(fixture.privacyRequests().size(), controls ? 1 : 0);
}

void SupervisorTests::transportLossCanResume_data()
{
    QTest::addColumn<bool>("pinned");
    QTest::addColumn<bool>("quitOnStop");
    QTest::addColumn<bool>("lost");
    QTest::newRow("authenticated-loss") << true << false << true;
    QTest::newRow("no-bound-certificate") << false << false << true;
    QTest::newRow("quit-on-stop") << true << true << true;
    QTest::newRow("normal-exit") << true << false << false;
}

void SupervisorTests::transportLossCanResume()
{
    QFETCH(bool, pinned);
    QFETCH(bool, quitOnStop);
    QFETCH(bool, lost);
    Fixture fixture;
    QVERIFY(fixture.install(lost ? "transport-loss" : "exit-on-fixture"));
    QWindow owner;
    SessionSupervisor session(fixture.program);
    QSignalSpy prepared(&session, &SessionSupervisor::prepared);
    QSignalSpy finished(&session, &SessionSupervisor::finished);
    const auto attempt = session.prepare(directCandidate());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.size(), 1, 3000);
    QVERIFY(preparedAt(prepared).verifiesHostIdentity);
    QVERIFY(session.start({attempt, generation, {}, pinned ? QByteArray(32, 'f') : QByteArray{}},
                          pictureView(owner, quitOnStop)));
    QTRY_COMPARE_WITH_TIMEOUT(session.phase(), SessionSupervisor::Phase::Streaming, 3000);

    QFile gate(fixture.exitPlayback);
    QVERIFY(gate.open(QIODevice::WriteOnly));
    gate.close();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3500);
    QCOMPARE(outcome(finished).reason, lost ? SessionEnd::TransportLost : SessionEnd::Exited);
    QCOMPARE(outcome(finished).canResumeAfterLoss, pinned && !quitOnStop && lost);
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

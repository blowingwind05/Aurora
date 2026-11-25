// SPDX-License-Identifier: GPL-3.0-only
#include "SystemPairingNc.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace aurora::tailscale;
using namespace aurora::tailscale::system;

namespace {

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (false)

const AccountIdentity Account{std::numeric_limits<qint64>::max(), "system-local", "system-tailnet"};
const QHostAddress Address("100.76.1.2");
const QHostAddress V6("fd7a:115c:a1e0::2");

template<typename Test>
bool waitFor(Test done, int milliseconds = 2500)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        if (!done())
            QThread::msleep(1);
    }
    return done();
}

struct Fixture
{
    Fixture(const QString &fake, const QString &mode)
        : directory(QDir::current().filePath("aurora-nc-XXXXXX")), executable(QDir(directory.path()).filePath(mode + ".exe")),
          relay([this] { return Installation{selected, service}; })
    {
        CHECK(directory.isValid() && QFile::copy(fake, executable));
        selected = executable;
        relay.setAuthorization(17, Account);
        QObject::connect(&relay, &SystemPairingNc::ready, &relay, [this](quint64 id, QIODevice *io) {
            readyId = id;
            channel = io;
        });
        QObject::connect(&relay, &SystemPairingNc::finished, &relay, [this](quint64 id, const PairingNcResult &result) {
            completedId = id;
            completed = result;
        });
    }

    QByteArray trace() const
    {
        QFile file(QDir(directory.path()).filePath("trace.txt"));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }

    PairingNcTarget target(const QHostAddress &address = Address) const
    {
        return {17, Account, "same-owner", address};
    }

    PairingNcResult result(quint64 id)
    {
        CHECK(id && waitFor([&] { return completedId == id; }));
        return *completed;
    }

    void awaitReady(quint64 id)
    {
        CHECK(id && waitFor([&] { return readyId == id || completedId == id; }));
        CHECK(readyId == id && channel && channel->isSequential() && channel->isOpen());
        CHECK(!qobject_cast<QProcess *>(channel.data()));
    }

    const QTemporaryDir directory;
    const QString executable;
    QString selected;
    ServiceState service = ServiceState::Running;
    quint64 readyId = 0;
    quint64 completedId = 0;
    QPointer<QIODevice> channel;
    std::optional<PairingNcResult> completed;
    SystemPairingNc relay;
};

void exchange(const QString &fake)
{
    qputenv("TS_SOCKET", "untrusted");
    qputenv("TS_AUTHKEY", "invalid-fixture-value");
    qputenv("HTTPS_PROXY", "invalid-fixture-value");
    qputenv("AURORA_NC_TEST_SECRET", "invalid-fixture-value");
    for (const auto &mode : {"binary", "ipv6", "exit-error"}) {
        const bool ipv6 = QByteArray(mode) == "ipv6";
        Fixture fixture(fake, mode);
        const auto id = fixture.relay.start(fixture.target(ipv6 ? V6 : QHostAddress("::ffff:100.76.1.2")));
        fixture.awaitReady(id);
        CHECK(fixture.relay.start(fixture.target()) == 0);
        QByteArray firstByte;
        QObject::connect(fixture.channel, &QIODevice::readyRead, &fixture.relay, [&] {
            if (firstByte.isEmpty())
                firstByte = fixture.channel->read(1);
        });
        CHECK(fixture.channel->write(QByteArray("a\r\n\0", 4)) == 4);
        const auto result = fixture.result(id);
        CHECK(result.exitCode == (QByteArray(mode) == "exit-error" ? 19 : 0));
        CHECK(result.error == (result.exitCode == 0 ? PairingNcError::None : PairingNcError::Transport));
        CHECK(fixture.channel->isReadable() && !fixture.channel->isWritable());
        CHECK(firstByte == "b" && fixture.channel->readAll() == QByteArray("\r\n\0", 3));
        CHECK(fixture.trace() == (ipv6 ? "status\nnc6\n" : "status\nnc4\n"));
    }
}

void guards(const QString &fake)
{
    for (const auto &mode : {"wrong-owner", "wrong-node", "wrong-tailnet", "wrong-peer", "wrong-address",
                              "ambiguous", "other-user", "tagged", "shared"}) {
        Fixture fixture(fake, mode);
        CHECK(fixture.result(fixture.relay.start(fixture.target())).error == PairingNcError::IdentityMismatch);
        CHECK(!fixture.readyId && fixture.trace() == "status\n");
    }

    Fixture fixture(fake, "binary");
    auto target = fixture.target();
    target.account.ownerId--;
    CHECK(fixture.relay.start(target) == 0);
    target = fixture.target();
    --target.generation;
    CHECK(fixture.relay.start(target) == 0);
    CHECK(fixture.relay.start(fixture.target(QHostAddress::LocalHost)) == 0);
    CHECK(fixture.relay.start(fixture.target(QHostAddress("fd7a:115c:a1e0::2%3"))) == 0);
    fixture.relay.setAuthorization(18, std::nullopt);
    fixture.relay.setAuthorization(17, Account);
    CHECK(fixture.relay.start(fixture.target()) == 0 && fixture.trace().isEmpty());
}

void lifecycle(const QString &fake)
{
    Fixture unavailable(fake, "binary");
    unavailable.service = ServiceState::Stopped;
    CHECK(unavailable.result(unavailable.relay.start(unavailable.target())).error == PairingNcError::InstallationUnavailable);
    CHECK(unavailable.trace().isEmpty());

    Fixture changing(fake, "binary");
    const auto replacement = QDir(changing.directory.path()).filePath("replacement.exe");
    CHECK(QFile::copy(fake, replacement));
    int lookups = 0;
    SystemPairingNc differentInstall([&] {
        return Installation{++lookups == 1 ? changing.executable : replacement, ServiceState::Running};
    });
    std::optional<PairingNcResult> changed;
    QObject::connect(&differentInstall, &SystemPairingNc::finished, &differentInstall,
                     [&](quint64, const PairingNcResult &result) { changed = result; });
    differentInstall.setAuthorization(17, Account);
    CHECK(differentInstall.start(changing.target()));
    CHECK(waitFor([&] { return changed.has_value(); }));
    CHECK(changed->error == PairingNcError::InstallationUnavailable && changing.trace() == "status\n");

    Fixture authorizing(fake, "status-hold");
    auto id = authorizing.relay.start(authorizing.target());
    CHECK(id && waitFor([&] { return authorizing.trace() == "status\n"; }));
    authorizing.relay.setAuthorization(18, std::nullopt);
    CHECK(authorizing.result(id).error == PairingNcError::AuthorizationChanged && !authorizing.readyId);
    CHECK(authorizing.trace() == "status\n");

    Fixture live(fake, "relay-hold");
    id = live.relay.start(live.target());
    live.awaitReady(id);
    live.relay.setAuthorization(18, std::nullopt);
    CHECK(live.result(id).error == PairingNcError::AuthorizationChanged && !live.channel->isOpen());

    Fixture cancelled(fake, "relay-hold");
    id = cancelled.relay.start(cancelled.target());
    cancelled.awaitReady(id);
    cancelled.channel->close();
    CHECK(cancelled.result(id).error == PairingNcError::Cancelled);
}

void limits(const QString &fake)
{
    for (const auto &mode : {"status-denied", "status-malformed"}) {
        Fixture failed(fake, mode);
        CHECK(failed.result(failed.relay.start(failed.target())).error == PairingNcError::StatusFailed);
        CHECK(!failed.readyId && failed.trace() == "status\n");
    }
    for (const auto &mode : {"stdout-flood", "stderr-flood"}) {
        Fixture flood(fake, mode);
        CHECK(flood.result(flood.relay.start(flood.target())).error == PairingNcError::LimitExceeded);
        CHECK(flood.channel && !flood.channel->isOpen());
    }

    Fixture slow(fake, "status-hold");
    const auto slowId = slow.relay.start(slow.target(), 1600);
    CHECK(slowId && waitFor([&] { return slow.trace() == "status\n"; }));
    CHECK(slow.result(slowId).error == PairingNcError::Timeout);
    CHECK(!slow.readyId && slow.trace() == "status\n");

    Fixture live(fake, "relay-hold");
    const auto id = live.relay.start(live.target(), 2000);
    live.awaitReady(id);
    CHECK(live.result(id).error == PairingNcError::Timeout && !live.channel->isOpen());

    Fixture input(fake, "relay-hold");
    const auto writeId = input.relay.start(input.target());
    input.awaitReady(writeId);
    CHECK(input.channel->write(QByteArray(65537, 'x')) == -1);
    CHECK(input.result(writeId).error == PairingNcError::LimitExceeded);
}

} // namespace

int main(int argc, char **argv)
{
    const QCoreApplication app(argc, argv);
    const QHash<QString, void (*)(const QString &)> tests{
        {"exchange", exchange}, {"guards", guards}, {"lifecycle", lifecycle}, {"limits", limits}
    };
    try {
        CHECK(app.arguments().size() == 3);
        const auto run = tests.value(app.arguments().at(1));
        CHECK(run);
        run(app.arguments().at(2));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "System nc fixture failed: %s\n", error.what());
        return 1;
    }
}

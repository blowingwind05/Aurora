// SPDX-License-Identifier: GPL-3.0-only
#include "WallpaperClient.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace Aurora;
using namespace Aurora::Wallpaper;

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #test); return false; \
} } while (false)

namespace {

const QString HostId = QStringLiteral("2706b864-5702-482e-a17b-34ebdb7db967");
const QHostAddress DirectAddress(QStringLiteral("192.0.2.8"));
const QHostAddress TailnetAddress(QStringLiteral("100.65.12.10"));
constexpr int RelayPort = 54321;

QByteArray image(const char *format, int width = 64, int height = 32, QRgb color = qRgb(18, 84, 137))
{
    QImage frame(width, height, QImage::Format_RGB32);
    frame.fill(color);
    QByteArray encoded;
    QBuffer output(&encoded);
    output.open(QIODevice::WriteOnly);
    return frame.save(&output, format) ? encoded : QByteArray{};
}

QByteArray atLimit(const QByteArray &jpeg, int size)
{
    // Add legal JPEG comment segments after SOI to exercise the exact protocol byte limits.
    const int maxComment = 65530;
    QByteArray padded = jpeg.left(2);
    int remaining = size - jpeg.size();
    while (remaining > 0) {
        const auto segment = remaining > maxComment ? qMin(maxComment, remaining - 4) : remaining;
        const auto length = segment - 2;
        padded += "\xff\xfe";
        padded += char(length >> 8);
        padded += char(length & 0xff);
        padded += QByteArray(segment - 4, 'A');
        remaining -= segment;
    }
    return padded + jpeg.mid(2);
}

QByteArray revision(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toUpper();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

template <typename Predicate>
bool waitUntil(Predicate ready, int timeoutMs = 4000)
{
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(4);
    }
    return ready();
}

class Fixture
{
public:
    explicit Fixture(const QString &program)
        : client(dataPath(), program)
    {
        device.id = QStringLiteral("lan:72db403b5c29410292caa2f146b23a70");
        device.lan.emplace();
        device.lan->endpoint = {DirectAddress, Discovery::BundledSunshinePort};
        device.lan->id = HostId;
        request.pinnedRoute = {DirectAddress, Discovery::BundledSunshinePort, HostId, false};
        QObject::connect(&client, &PreviewClient::finished, &client, [this](quint64 id, FetchResult result) {
            results.append({id, result});
        });
        request.authorization = client.setContext(1, true);
    }

    QString dataPath() const { return data.filePath(QStringLiteral("Aurora")); }
    QString launchedPath() const { return data.filePath(QStringLiteral("launched")); }
    QString gatePath() const { return data.filePath(QStringLiteral("gate")); }
    QString cachePath(const Discovery::NetworkSnapshot *network = nullptr) const
    {
        return previewCachePath(dataPath(), device, network);
    }

    bool reply(const QByteArray &output, const QByteArray &expectedRevision, const QByteArray &mode = {})
    {
        const auto payload = data.filePath(QStringLiteral("payload"));
        if (!writeFile(payload, output))
            return false;

        QFile::remove(launchedPath());
        QFile::remove(gatePath());
        const auto &route = request.pinnedRoute;
        const auto port = QByteArray::number(route.port);
        qputenv("WALLPAPER_TEST_LAUNCHED", launchedPath().toUtf8());
        qputenv("WALLPAPER_TEST_GATE", gatePath().toUtf8());
        qputenv("WALLPAPER_TEST_PAYLOAD", payload.toUtf8());
        qputenv("WALLPAPER_TEST_MODE", mode);
        qputenv("WALLPAPER_TEST_ENDPOINT", route.address.toString().toUtf8() + ':' + port);
        qputenv("WALLPAPER_TEST_HOST", route.hostId.toUtf8());
        qputenv("WALLPAPER_TEST_REVISION", expectedRevision);
        qputenv("WALLPAPER_TEST_RELAY_BASE", route.viaEmbeddedTailnet ? port : QByteArray{});
        qputenv("WALLPAPER_TEST_RELAY_HOST", route.viaEmbeddedTailnet ? route.hostId.toUtf8() : QByteArray{});
        qputenv("AURORA_GAMESTREAM_RELAY_BASE", "60000");
        qputenv("AURORA_GAMESTREAM_HOST_UUID", "stale-host");
        return true;
    }

    bool received(quint64 id, FetchResult result, int timeoutMs = 4000)
    {
        return id && waitUntil([this, id] { return !results.isEmpty() && results.last().first == id; }, timeoutMs)
            && results.last().second == result;
    }

    QTemporaryDir data;
    PreviewClient client;
    Discovery::Device device;
    FetchRequest request;
    QList<QPair<quint64, FetchResult>> results;
};

bool cache(const QString &program)
{
    Fixture test(program);
    CHECK(test.data.isValid());
    const auto lanPath = test.cachePath();
    CHECK(QFileInfo(lanPath).fileName() == "C463B1A5377EC27A87C09590C31F38FC904592594648D8B83EA70A07FBF639B1.jpg");

    // Merged tailnet IDs keep their account-scoped filenames even when using a LAN route.
    test.device.tailnetVisible = true;
    test.device.id = QString::fromStdU16String(u"n\u0153ud-\u8bbe\u5907");
    Discovery::NetworkSnapshot network{TailnetAddress, 42, QStringLiteral("node-local"), {}};
    const auto path = test.cachePath(&network);
    CHECK(QFileInfo(path).fileName() == "2459AD8D77AD0A36B566711BB1810071443FE771158354C3A63A47BE5E1EACD1.jpg");
    CHECK(test.cachePath().isEmpty());
    CHECK(existingPreviewPath(test.dataPath(), test.device, &network).isEmpty());
    auto otherAccount = network;
    ++otherAccount.owner;
    CHECK(test.cachePath(&otherAccount) != path);
    otherAccount = network;
    otherAccount.localId = QStringLiteral("other-node");
    CHECK(test.cachePath(&otherAccount) != path);

    const auto legacy = image("jpeg");
    CHECK(!legacy.isEmpty());
    CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
    CHECK(writeFile(path, legacy));
    const auto modified = QFileInfo(path).lastModified();
    CHECK(existingPreviewPath(test.dataPath(), test.device, &network) == path);
    CHECK(existingPreviewPath(test.dataPath(), test.device, &otherAccount).isEmpty());
    CHECK(test.reply("=\n", revision(legacy)));
    CHECK(test.received(test.client.start(test.device, &network, test.request), FetchResult::Unchanged));
    CHECK(readFile(path) == legacy && QFileInfo(path).lastModified() == modified);

    const auto updated = image("jpeg", MaxPreviewEdge, 32, qRgb(31, 92, 170));
    CHECK(!updated.isEmpty() && updated != legacy);
    CHECK(test.reply(updated.toBase64() + "\n", revision(legacy)));
    CHECK(test.received(test.client.start(test.device, &network, test.request), FetchResult::Updated));
    CHECK(readFile(path) == updated);
    CHECK(!QFile::exists(lanPath));
    return true;
}

bool protocol(const QString &program)
{
    Fixture test(program);
    const auto jpeg = image("jpeg");
    CHECK(!jpeg.isEmpty());
    const auto limit = atLimit(jpeg, MaxPreviewBytes);
    CHECK(limit.size() == MaxPreviewBytes);
    CHECK(test.reply(limit.toBase64() + "\n", "-"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::Updated));
    CHECK(readFile(test.cachePath()) == limit);

    const auto rev = revision(limit);
    const auto invalid = {
        QByteArray{},
        QByteArray("not@base64\n"),
        image("png").toBase64() + "\n",
        image("jpeg", MaxPreviewEdge + 1).toBase64() + "\n",
        jpeg.chopped(2).toBase64() + "\n",
        (jpeg.left(2) + QByteArray("\xff\xd9", 2)).toBase64() + "\n",
        atLimit(jpeg, MaxPreviewBytes + 1).toBase64() + "\n"
    };
    for (const auto &output : invalid) {
        CHECK(test.reply(output, rev));
        CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::InvalidResponse));
        CHECK(readFile(test.cachePath()) == limit);
    }
    CHECK(test.reply({}, rev, "overflow"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::InvalidResponse));
    CHECK(test.reply(jpeg.toBase64() + "\n", rev, "fail"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::Unavailable));
    CHECK(readFile(test.cachePath()) == limit);
    return true;
}

bool storage(const QString &program)
{
    Fixture test(program);
    const auto path = test.cachePath();
    const auto directory = QFileInfo(path).absolutePath();
    const auto jpeg = image("jpeg");
    CHECK(!jpeg.isEmpty());

    // A failed fetch must leave an empty profile untouched.
    CHECK(test.reply("=\n", "-"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::Unchanged));
    CHECK(!QFileInfo::exists(test.dataPath()));

    CHECK(QDir().mkpath(path));
    CHECK(test.reply(jpeg.toBase64() + "\n", "-"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::CacheWriteFailed));
    CHECK(QFileInfo(path).isDir());
    CHECK(QDir(directory).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size() == 1);
    CHECK(QDir().rmdir(path));

    const QByteArray oversized(MaxPreviewBytes + 1, 'A');
    CHECK(writeFile(path, oversized));
    CHECK(existingPreviewPath(test.dataPath(), test.device).isEmpty());
    CHECK(test.reply(jpeg.toBase64() + "\n", "-"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::Updated));
    CHECK(readFile(path) == jpeg);

#ifdef Q_OS_WIN
    const auto replacement = image("jpeg", 32, 64);
    CHECK(!replacement.isEmpty() && test.reply(replacement.toBase64() + "\n", revision(jpeg)));
    const auto nativePath = QDir::toNativeSeparators(path);
    const auto held = CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const bool replaceBlocked = test.received(test.client.start(test.device, nullptr, test.request),
                                               FetchResult::CacheWriteFailed);
    CloseHandle(held);
    CHECK(replaceBlocked);
    CHECK(readFile(path) == jpeg);
    CHECK(QDir(directory).entryList(QDir::Files).size() == 1);
#endif

#ifdef Q_OS_UNIX
    const auto outside = test.data.filePath(QStringLiteral("outside"));
    CHECK(writeFile(outside, "untouched"));
    CHECK(QFile::remove(path));
    CHECK(QFile::link(outside, path));
    CHECK(existingPreviewPath(test.dataPath(), test.device).isEmpty());
    CHECK(test.reply(jpeg.toBase64() + "\n", "-"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::CacheWriteFailed));
    CHECK(QFileInfo(path).isSymLink() && readFile(outside) == "untouched");
    CHECK(QFile::remove(path));
    CHECK(QDir().rmdir(directory));

    const auto redirected = test.data.filePath(QStringLiteral("redirected"));
    CHECK(QDir().mkdir(redirected));
    CHECK(QFile::link(redirected, directory));
    CHECK(test.reply(jpeg.toBase64() + "\n", "-"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::CacheWriteFailed));
    CHECK(QDir(redirected).isEmpty());
#endif
    return true;
}

bool authorization(const QString &program)
{
    Fixture test(program);
    const auto jpeg = image("jpeg");
    CHECK(!jpeg.isEmpty());
    CHECK(test.reply(jpeg.toBase64() + "\n", "-"));

    auto wrongRoute = test.request;
    wrongRoute.pinnedRoute.hostId = "unexpected-host";
    CHECK(!test.client.start(test.device, nullptr, wrongRoute));
    wrongRoute = test.request;
    wrongRoute.pinnedRoute.address = TailnetAddress;
    CHECK(!test.client.start(test.device, nullptr, wrongRoute));

    const auto queued = test.client.start(test.device, nullptr, test.request);
    CHECK(queued && !test.client.start(test.device, nullptr, test.request));
    CHECK(!test.client.setContext(1, false));
    CHECK(test.received(queued, FetchResult::Cancelled));
    const auto newAuthorization = test.client.setContext(1, true);
    CHECK(!test.client.start(test.device, nullptr, test.request));
    CHECK(!waitUntil([&test] { return QFile::exists(test.launchedPath()); }, 150));
    CHECK(!QFileInfo::exists(test.dataPath()));

    test.request.authorization = newAuthorization;
    const auto path = test.cachePath();
    CHECK(QDir().mkpath(QFileInfo(path).absolutePath()));
    const auto cached = image("jpeg", 32, 64);
    CHECK(!cached.isEmpty() && writeFile(path, cached));
    CHECK(test.reply(jpeg.toBase64() + "\n", revision(cached), "hold"));
    const auto running = test.client.start(test.device, nullptr, test.request);
    CHECK(running && waitUntil([&test] { return QFile::exists(test.launchedPath()); }));
    test.client.setContext(2, false);
    CHECK(test.received(running, FetchResult::Cancelled));
    CHECK(writeFile(test.gatePath(), "go"));
    CHECK(!waitUntil([&test, &path, &cached] { return test.results.size() != 2 || readFile(path) != cached; }, 150));
    CHECK(QDir(QFileInfo(path).absolutePath()).entryList(QDir::Files) == QStringList{QFileInfo(path).fileName()});

    test.request.authorization = test.client.setContext(2, true);
    test.device.tailnet.emplace();
    test.device.tailnet->endpoint = {TailnetAddress, Discovery::BundledSunshinePort};
    test.device.tailnet->id = HostId;
    test.device.tailnet->viaEmbeddedTailnet = true;
    test.request.pinnedRoute = {QHostAddress::LocalHost, RelayPort, HostId, true};
    CHECK(!test.client.start(test.device, nullptr, test.request));
    auto lease = std::make_unique<QObject>();
    test.request.routeLifetime = lease.get();
    CHECK(test.reply(jpeg.toBase64() + "\n", revision(cached), "hold"));
    const auto relay = test.client.start(test.device, nullptr, test.request);
    CHECK(relay && waitUntil([&test] { return QFile::exists(test.launchedPath()); }));
    lease.reset();
    CHECK(test.received(relay, FetchResult::Cancelled));
    CHECK(writeFile(test.gatePath(), "go"));
    CHECK(!waitUntil([&test, &path, &cached] { return test.results.size() != 3 || readFile(path) != cached; }, 150));

    lease = std::make_unique<QObject>();
    test.request.routeLifetime = lease.get();
    CHECK(test.reply(jpeg.toBase64() + "\n", revision(cached)));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::Updated));
    CHECK(readFile(path) == jpeg);

    CHECK(test.reply("=\n", revision(jpeg)));
    const auto pending = test.client.start(test.device, nullptr, test.request);
    CHECK(pending);
    CHECK(test.client.setContext(3, true) != test.request.authorization);
    CHECK(test.received(pending, FetchResult::Cancelled));
    CHECK(!test.client.start(test.device, nullptr, test.request));
    CHECK(!waitUntil([&test] { return QFile::exists(test.launchedPath()); }, 150));
    return true;
}

bool timeout(const QString &program)
{
    Fixture test(program);
    CHECK(test.reply("=\n", "-", "hold"));
    const auto id = test.client.start(test.device, nullptr, test.request);
    CHECK(test.received(id, FetchResult::TimedOut, 6500));
    CHECK(QFile::exists(test.launchedPath()));
    CHECK(!QFileInfo::exists(test.dataPath()));
    CHECK(test.reply("=\n", "-"));
    CHECK(test.received(test.client.start(test.device, nullptr, test.request), FetchResult::Unchanged));
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3)
        return 2;
    if (Stream::currentPlatform() == Stream::RuntimePlatform::Unsupported)
        return 77;

    const auto name = args.at(1);
    const auto program = args.at(2);
    if (name == "cache")
        return cache(program) ? 0 : 1;
    if (name == "protocol")
        return protocol(program) ? 0 : 1;
    if (name == "storage")
        return storage(program) ? 0 : 1;
    if (name == "authorization")
        return authorization(program) ? 0 : 1;
    if (name == "timeout")
        return timeout(program) ? 0 : 1;
    return 2;
}

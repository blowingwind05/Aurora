// SPDX-License-Identifier: GPL-3.0-only
#include "WallpaperClient.h"

#include "MoonlightProcess.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cstdio>
#include <sys/stat.h>
#endif

namespace Aurora::Wallpaper {
namespace {

constexpr int TimeoutMs = 5000;
constexpr int MaxBase64Bytes = ((MaxPreviewBytes + 2) / 3) * 4;
constexpr int MaxStdoutBytes = MaxBase64Bytes + 2; // CRLF from the Windows runtime.

QString localDataDirectory(const QString &path)
{
    const auto normalized = QDir::fromNativeSeparators(path);
    if (normalized.contains(QChar::Null))
        return {};
#ifdef Q_OS_WIN
    if (normalized.size() < 3 || normalized.mid(1, 2) != QLatin1String(":/"))
        return {};
    const auto drive = normalized.at(0).toUpper();
    if (drive < QLatin1Char('A') || drive > QLatin1Char('Z'))
        return {};
#else
    if (!normalized.startsWith(QLatin1Char('/')) || normalized.startsWith(QLatin1String("//")))
        return {};
#endif
    return QDir::cleanPath(normalized);
}

QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toUpper());
}

bool redirected(const QFileInfo &entry)
{
#ifdef Q_OS_WIN
    return entry.isSymLink() || entry.isJunction();
#else
    return entry.isSymLink();
#endif
}

bool usableDirectory(const QString &path)
{
    const QFileInfo entry(path);
    return entry.isDir() && !redirected(entry);
}

bool usableEntry(const QString &dataDirectory, const QString &path)
{
    const QFileInfo entry(path);
    return !path.isEmpty() && usableDirectory(dataDirectory) && usableDirectory(entry.absolutePath()) &&
        entry.isFile() && !redirected(entry) && entry.size() > 0 && entry.size() <= MaxPreviewBytes;
}

QString cachedRevision(const QString &dataDirectory, const QString &path)
{
    if (!usableEntry(dataDirectory, path))
        return {};

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto bytes = file.read(MaxPreviewBytes + 1);
    return bytes.isEmpty() || bytes.size() > MaxPreviewBytes || file.error() != QFileDevice::NoError
        ? QString() : digest(bytes);
}

bool prepareCacheDirectory(const QString &dataDirectory, const QString &cacheDirectory)
{
    if (redirected(QFileInfo(dataDirectory)) || !QDir().mkpath(dataDirectory) ||
        !usableDirectory(dataDirectory) || redirected(QFileInfo(cacheDirectory))) {
        return false;
    }
#ifdef Q_OS_WIN
    QDir().mkdir(cacheDirectory);
#else
    // Keep new preview directories private. An existing legacy directory keeps its permissions.
    ::mkdir(QFile::encodeName(cacheDirectory).constData(), S_IRWXU);
#endif
    return usableDirectory(cacheDirectory);
}

bool safeDestination(const QString &path)
{
    const QFileInfo entry(path);
    return !redirected(entry) && (!entry.exists() || entry.isFile());
}

bool replaceFile(const QString &temporary, const QString &path)
{
#ifdef Q_OS_WIN
    const auto from = QDir::toNativeSeparators(temporary);
    const auto to = QDir::toNativeSeparators(path);
    return MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) == 0;
#endif
}

bool savePreview(const QString &dataDirectory, const QString &path, const QByteArray &jpeg,
                 const QDeadlineTimer &deadline)
{
    const auto directory = QFileInfo(path).absolutePath();
    if (!prepareCacheDirectory(dataDirectory, directory) || !safeDestination(path))
        return false;

    QString temporary;
    {
        QTemporaryFile file(path + QStringLiteral(".XXXXXX"));
        if (!file.open() || file.write(jpeg) != jpeg.size() || !file.flush())
            return false;
        temporary = file.fileName();
        file.setAutoRemove(false);
    }

    // Replace the exact cache entry without opening an existing file or following a file link.
    if (!deadline.hasExpired() && usableDirectory(dataDirectory) && usableDirectory(directory)
        && safeDestination(path) && replaceFile(temporary, path)) {
        return true;
    }
    QFile::remove(temporary);
    return false;
}

bool validJpeg(const QByteArray &bytes)
{
    if (bytes.size() > MaxPreviewBytes || !bytes.startsWith("\xff\xd8") || !bytes.endsWith("\xff\xd9"))
        return false;

    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "jpeg");
    reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    return !size.isEmpty() && size.width() <= MaxPreviewEdge && size.height() <= MaxPreviewEdge
        && !reader.read().isNull();
}

bool matchesDevice(const Discovery::Device &device, const Stream::Route &route)
{
    for (const auto *candidate : {&device.lan, &device.tailnet}) {
        if (!*candidate)
            continue;
        const auto &host = **candidate;
        if (host.viaEmbeddedTailnet == route.viaEmbeddedTailnet && Discovery::sameHostIdentity(host.id, route.hostId)
            && (route.viaEmbeddedTailnet || (host.endpoint.address == route.address && host.endpoint.port == route.port))) {
            return true;
        }
    }
    return false;
}

} // namespace

QString previewCachePath(const QString &dataDirectory, const Discovery::Device &device,
                         const Discovery::NetworkSnapshot *network)
{
    const auto root = localDataDirectory(dataDirectory);
    if (root.isEmpty() || device.id.isEmpty())
        return {};

    auto identity = device.id;
    if (device.tailnetVisible) {
        if (!network || !network->isAuthenticated())
            return {};
        identity = QStringLiteral("tailnet:%1:%2:%3").arg(QString::number(network->owner), network->localId, device.id);
    }
    return QDir(root).filePath(QStringLiteral("Wallpapers/") + digest(identity.toUtf8()) + QStringLiteral(".jpg"));
}

QString existingPreviewPath(const QString &dataDirectory, const Discovery::Device &device,
                            const Discovery::NetworkSnapshot *network)
{
    const auto path = previewCachePath(dataDirectory, device, network);
    return usableEntry(localDataDirectory(dataDirectory), path) ? path : QString();
}

struct PreviewClient::Attempt : QObject
{
    Attempt(PreviewClient *owner, quint64 requestId, QString cachePath)
        : QObject(owner), id(requestId), path(std::move(cachePath))
    {
        timer.setSingleShot(true);
        timer.setTimerType(Qt::PreciseTimer);
    }

    const quint64 id;
    const QString path;
    QDeadlineTimer deadline{TimeoutMs, Qt::PreciseTimer};
    QTimer timer;
    Stream::MoonlightProcess process;
    QByteArray output;
    QMetaObject::Connection leaseClosed;
};

PreviewClient::PreviewClient(QString dataDirectory, QString moonlightProgram, QObject *parent)
    : QObject(parent), dataDirectory_(localDataDirectory(dataDirectory)), moonlightProgram_(std::move(moonlightProgram))
{
}

quint64 PreviewClient::setContext(quint64 epoch, bool foreground)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (epoch == epoch_ && foreground == foreground_)
        return foreground ? authorization_ : 0;

    epoch_ = epoch;
    foreground_ = foreground;
    const auto authorization = ++authorization_;
    if (active_)
        complete(active_, FetchResult::Cancelled);
    return foreground ? authorization : 0;
}

quint64 PreviewClient::start(const Discovery::Device &device, const Discovery::NetworkSnapshot *network,
                             const FetchRequest &request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!foreground_ || request.authorization != authorization_ || active_
        || !matchesDevice(device, request.pinnedRoute)
        || (request.pinnedRoute.viaEmbeddedTailnet && !request.routeLifetime)
        || (request.routeLifetime && request.routeLifetime->thread() != thread())) {
        return 0;
    }

    const auto path = previewCachePath(dataDirectory_, device, network);
    if (path.isEmpty())
        return 0;

    const auto id = ++nextRequest_;
    auto *attempt = new Attempt(this, id, path);
    active_ = attempt;
    if (request.routeLifetime) {
        attempt->leaseClosed = connect(request.routeLifetime, &QObject::destroyed, this, [this, attempt] {
            complete(attempt, FetchResult::Cancelled);
        });
    }

    auto &process = attempt->process;
    connect(&process, &QProcess::readyReadStandardOutput, this, [this, attempt] { collect(attempt); });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, attempt](int code, QProcess::ExitStatus status) {
        processFinished(attempt, status == QProcess::NormalExit && code == 0);
    });
    connect(&process, &QProcess::errorOccurred, this, [this, attempt](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart || error == QProcess::ReadError)
            complete(attempt, FetchResult::Unavailable);
    });
    connect(&attempt->timer, &QTimer::timeout, this, [this, attempt] { complete(attempt, FetchResult::TimedOut); });
    attempt->timer.start(TimeoutMs);

    Stream::Request command;
    command.operation = Stream::Operation::Wallpaper;
    command.route = request.pinnedRoute;
    QTimer::singleShot(0, attempt, [this, attempt, command] { launch(attempt, command); });
    return id;
}

void PreviewClient::cancel(quint64 request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (active_ && active_->id == request)
        complete(active_, FetchResult::Cancelled);
}

void PreviewClient::launch(Attempt *attempt, Stream::Request request)
{
    if (active_ != attempt)
        return;
    request.wallpaperRevision = cachedRevision(dataDirectory_, attempt->path);
    const auto command = Stream::makeCommand(moonlightProgram_, request);
    if (attempt->deadline.hasExpired())
        complete(attempt, FetchResult::TimedOut);
    else if (!command.command || !attempt->process.launch(*command.command))
        complete(attempt, FetchResult::Unavailable);
}

bool PreviewClient::collect(Attempt *attempt)
{
    if (active_ != attempt)
        return false;

    const auto remaining = MaxStdoutBytes - attempt->output.size();
    if (attempt->process.bytesAvailable() > remaining) {
        complete(attempt, FetchResult::InvalidResponse);
        return false;
    }

    attempt->output += attempt->process.read(remaining);
    return true;
}

void PreviewClient::processFinished(Attempt *attempt, bool success)
{
    if (!collect(attempt))
        return;
    if (attempt->deadline.hasExpired()) {
        complete(attempt, FetchResult::TimedOut);
        return;
    }
    if (!success) {
        complete(attempt, FetchResult::Unavailable);
        return;
    }

    const auto payload = attempt->output.trimmed();
    if (payload == "=") {
        complete(attempt, FetchResult::Unchanged);
        return;
    }

    if (payload.size() > MaxBase64Bytes) {
        complete(attempt, FetchResult::InvalidResponse);
        return;
    }
    const auto jpeg = QByteArray::fromBase64(payload, QByteArray::AbortOnBase64DecodingErrors);
    if (!validJpeg(jpeg) || jpeg.toBase64() != payload) {
        complete(attempt, FetchResult::InvalidResponse);
        return;
    }
    if (attempt->deadline.hasExpired()) {
        complete(attempt, FetchResult::TimedOut);
        return;
    }
    const bool saved = savePreview(dataDirectory_, attempt->path, jpeg, attempt->deadline);
    if (saved)
        complete(attempt, FetchResult::Updated);
    else
        complete(attempt, attempt->deadline.hasExpired() ? FetchResult::TimedOut : FetchResult::CacheWriteFailed);
}

void PreviewClient::complete(Attempt *attempt, FetchResult result)
{
    if (active_ != attempt)
        return;
    active_ = nullptr;
    disconnect(attempt->leaseClosed);
    attempt->timer.stop();
    attempt->process.cancel();
    attempt->deleteLater();
    emit finished(attempt->id, result);
}

} // namespace Aurora::Wallpaper

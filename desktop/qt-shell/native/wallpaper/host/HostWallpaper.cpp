// SPDX-License-Identifier: GPL-3.0-only
#include "HostWallpaper.h"

#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QRunnable>
#include <QSaveFile>
#include <QThread>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <cerrno>
#include <sys/stat.h>
#endif

namespace Aurora::Wallpaper {

#ifdef Q_OS_MACOS
HostWallpaperResult encodePlatformPreview(const QByteArray &input, QByteArray &jpeg);
#endif

namespace {

using Cache = HostWallpaperCache;
using Result = HostWallpaperResult;
constexpr int updateIntervalMs = 45 * 1000;

QString localAbsolutePath(const QString &path)
{
    auto normalized = QDir::fromNativeSeparators(path);
    if (normalized.contains(QChar::Null))
        return {};
#ifdef Q_OS_WIN
    if (normalized.startsWith(QStringLiteral("//?/")))
        normalized.remove(0, 4);
    if (normalized.size() < 3 || normalized.mid(1, 2) != QLatin1String(":/"))
        return {};
    const auto drive = normalized.at(0).toUpper();
    if (drive < QLatin1Char('A') || drive > QLatin1Char('Z'))
        return {};
    const auto root = QDir::toNativeSeparators(normalized.left(3)).toStdWString();
    if (GetDriveTypeW(root.c_str()) == DRIVE_REMOTE)
        return {};
#else
    if (!normalized.startsWith(QLatin1Char('/')) || normalized.startsWith(QStringLiteral("//")))
        return {};
#endif
    return QDir::cleanPath(normalized);
}

QString outputPath(const QString &profile)
{
    const auto local = localAbsolutePath(profile);
    if (local.isEmpty() || QDir(local).isRoot())
        return {};
    return QDir(local).filePath(QStringLiteral("aurora-wallpaper.jpg"));
}

bool prepareOutputDirectory(const QString &directory)
{
#ifdef Q_OS_MACOS
    if (!QDir().mkpath(QFileInfo(directory).absolutePath()))
        return false;
    if (::mkdir(QFile::encodeName(directory).constData(), S_IRWXU) != 0 && errno != EEXIST)
        return false;
#else
    if (!QDir().mkpath(directory))
        return false;
#endif
    const QFileInfo profile(directory);
    return profile.isDir() && !profile.isSymLink();
}

bool savePreview(const QString &output, const QByteArray &jpeg)
{
    if (!prepareOutputDirectory(QFileInfo(output).absolutePath()))
        return false;
    const QFileInfo existing(output);
    if (existing.isSymLink() || (existing.exists() && !existing.isFile()))
        return false;

    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return false;
#ifdef Q_OS_MACOS
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
#endif
    return file.write(jpeg) == jpeg.size() && file.commit();
}

Result encodePreview(const QByteArray &input, QByteArray &jpeg)
{
    QBuffer buffer;
    buffer.setData(input);
    if (!buffer.open(QIODevice::ReadOnly))
        return Result::InvalidImage;
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(false);
    if (reader.format().isEmpty()) {
#ifdef Q_OS_MACOS
        return encodePlatformPreview(input, jpeg);
#else
        return Result::InvalidImage;
#endif
    }

    const auto size = reader.size();
    if (size.isEmpty())
        return Result::InvalidImage;
    if (qint64(size.width()) * size.height() > Cache::maxSourcePixels)
        return Result::InputTooLarge;

    const int edge = std::max(size.width(), size.height());
    QSize target = size;
    if (edge > Cache::maxPreviewEdge) {
        const auto scaledEdge = [edge](int n) {
            return std::max(1, int(qint64(n) * Cache::maxPreviewEdge / edge));
        };
        target = QSize(scaledEdge(size.width()), scaledEdge(size.height()));
        reader.setScaledSize(target);
    }

    auto image = reader.read();
    if (image.isNull())
        return Result::InvalidImage;
    if (image.size() != target)
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_RGB32);
    if (image.isNull())
        return Result::InvalidImage;

    // Encode the bitmap without retaining textual metadata from the source file.
    QImage pixels(image.constBits(), image.width(), image.height(), image.bytesPerLine(), image.format());
    pixels.setColorSpace(image.colorSpace());
    QBuffer output(&jpeg);
    if (!output.open(QIODevice::WriteOnly))
        return Result::InvalidImage;
    QImageWriter writer(&output, "jpeg");
    writer.setQuality(Cache::jpegQuality);
    if (!writer.write(pixels) || jpeg.isEmpty())
        return Result::InvalidImage;
    return jpeg.size() <= Cache::maxPreviewBytes ? Result::Updated : Result::OutputTooLarge;
}

bool sameFilePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

} // namespace

HostWallpaperCache::HostWallpaperCache(const QString &bundledProfileDirectory)
    : output_(outputPath(bundledProfileDirectory))
{
}

HostWallpaperResult HostWallpaperCache::refreshFromFile(const QString &sourceFile)
{
    if (output_.isEmpty())
        return Result::WriteFailed;
    const auto path = localAbsolutePath(sourceFile);
    if (path.isEmpty())
        return Result::NoSource;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0)
        return Result::NoSource;
    if (info.size() > maxInputBytes)
        return Result::InputTooLarge;

    const auto canonicalSource = info.canonicalFilePath();
    if (canonicalSource.isEmpty() || localAbsolutePath(canonicalSource).isEmpty())
        return Result::NoSource;
    const Source source{canonicalSource, info.lastModified().toUTC(), info.size()};

    const QFileInfo existing(output_);
    if (existing.isSymLink() || (existing.exists() && !existing.isFile())
        || sameFilePath(canonicalSource, existing.canonicalFilePath())) {
        return Result::WriteFailed;
    }
    if (lastSource_ && *lastSource_ == source && existing.isFile()) {
        return Result::Unchanged;
    }

    QFile file(canonicalSource);
    if (!file.open(QIODevice::ReadOnly))
        return Result::NoSource;
    const auto input = file.read(source.size + 1);
    if (input.size() > maxInputBytes)
        return Result::InputTooLarge;
    if (file.error() != QFileDevice::NoError || input.size() != source.size)
        return Result::NoSource;
    file.close();

    QByteArray jpeg;
    const auto encoded = encodePreview(input, jpeg);
    if (encoded != Result::Updated)
        return encoded;
    const QFileInfo after(path);
    if (after.canonicalFilePath() != source.path || after.size() != source.size
        || after.lastModified().toUTC() != source.modified) {
        return Result::NoSource;
    }

    if (!savePreview(output_, jpeg))
        return Result::WriteFailed;
    lastSource_ = source;
    return Result::Updated;
}

HostWallpaperUpdater::HostWallpaperUpdater(const QString &bundledProfileDirectory, QObject *parent)
    : HostWallpaperUpdater(bundledProfileDirectory, currentDesktopWallpaperFile, parent)
{
}

HostWallpaperUpdater::HostWallpaperUpdater(const QString &bundledProfileDirectory, SourceFile source, QObject *parent)
    : QObject(parent), cache_(bundledProfileDirectory), source_(std::move(source))
{
    qRegisterMetaType<HostWallpaperResult>();
    worker_.setMaxThreadCount(1);
}

HostWallpaperUpdater::~HostWallpaperUpdater()
{
    worker_.waitForDone();
}

bool HostWallpaperUpdater::refreshIfAllowed(bool foregroundHostRunning)
{
    Q_ASSERT(thread() == QThread::currentThread());
    if (!foregroundHostRunning || !source_ || busy_ ||
        (lastAttempt_.isValid() && lastAttempt_.elapsed() < updateIntervalMs)) {
        return false;
    }

    lastAttempt_.start();
    busy_ = true;
    const auto path = source_();
    worker_.start(QRunnable::create([this, path] {
        const auto result = cache_.refreshFromFile(path);
        QMetaObject::invokeMethod(this, [this, result] {
            busy_ = false;
            emit finished(result);
        }, Qt::QueuedConnection);
    }));
    return true;
}

} // namespace Aurora::Wallpaper

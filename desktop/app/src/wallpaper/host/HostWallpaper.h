// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <functional>
#include <optional>

namespace Aurora::Wallpaper {

enum class HostWallpaperResult { Updated, Unchanged, NoSource, InputTooLarge, InvalidImage,
                                 OutputTooLarge, WriteFailed };

// Read-only platform lookup. Call from the GUI thread.
QString currentDesktopWallpaperFile();

// File-only operation for a dedicated worker or an offline test. This object
// remembers the last source it successfully wrote and needs serialized calls.
class HostWallpaperCache final
{
public:
    static constexpr qint64 maxInputBytes = 80 * 1024 * 1024;
    static constexpr qint64 maxSourcePixels = 100'000'000;
    static constexpr int maxPreviewEdge = 960;
    static constexpr int maxPreviewBytes = 512 * 1024;
    static constexpr int jpegQuality = 78;

    explicit HostWallpaperCache(const QString &bundledProfileDirectory);
    const QString &wallpaperPath() const { return output_; }
    HostWallpaperResult refreshFromFile(const QString &sourceFile);

private:
    struct Source
    {
        QString path;
        QDateTime modified;
        qint64 size;

        bool operator==(const Source &other) const
        {
            return path == other.path && modified == other.modified && size == other.size;
        }
    };

    const QString output_;
    std::optional<Source> lastSource_;
};

// The caller decides when the bundled host is running in the foreground.
// Source lookup is synchronous on the GUI thread, and conversion and saving
// run on a private worker.
class HostWallpaperUpdater final : public QObject
{
    Q_OBJECT

public:
    using SourceFile = std::function<QString()>;

    explicit HostWallpaperUpdater(const QString &bundledProfileDirectory, QObject *parent = nullptr);
    HostWallpaperUpdater(const QString &bundledProfileDirectory, SourceFile source, QObject *parent = nullptr);
    ~HostWallpaperUpdater() override;

    // A true return schedules one completion. Requests are limited to one
    // active update and at most one attempt every 45 seconds.
    bool refreshIfAllowed(bool foregroundHostRunning);
    const QString &wallpaperPath() const { return cache_.wallpaperPath(); }

signals:
    void finished(Aurora::Wallpaper::HostWallpaperResult result);

private:
    HostWallpaperCache cache_;
    const SourceFile source_;
    QThreadPool worker_;
    QElapsedTimer lastAttempt_;
    bool busy_ = false;
};

} // namespace Aurora::Wallpaper

Q_DECLARE_METATYPE(Aurora::Wallpaper::HostWallpaperResult)

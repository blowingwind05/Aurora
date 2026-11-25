// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Discovery.h"
#include "MoonlightCommand.h"

#include <QObject>
#include <QPointer>

namespace Aurora::Wallpaper {

inline constexpr int MaxPreviewBytes = 512 * 1024;
inline constexpr int MaxPreviewEdge = 960;

// dataDirectory is the Aurora application-data directory. For tailnet devices,
// pass the same authenticated account used to build the merged device catalog.
QString previewCachePath(const QString &dataDirectory, const Discovery::Device &device,
                         const Discovery::NetworkSnapshot *network = nullptr);

// Returns a local cache file if a bounded, regular entry already exists.
QString existingPreviewPath(const QString &dataDirectory, const Discovery::Device &device,
                            const Discovery::NetworkSnapshot *network = nullptr);

struct FetchRequest
{
    quint64 authorization = 0;
    // Caller supplies the revalidated endpoint and host ID with an existing Moonlight pin.
    Stream::Route pinnedRoute;
    // Keep the embedded relay lease alive here. Destroying the token revokes the route.
    QPointer<QObject> routeLifetime;
};

enum class FetchResult { Updated, Unchanged, Cancelled, TimedOut, Unavailable, InvalidResponse, CacheWriteFailed };

// Own and call this object on the coordinator's Qt event-loop thread.
class PreviewClient final : public QObject
{
    Q_OBJECT

public:
    explicit PreviewClient(QString dataDirectory, QString moonlightProgram, QObject *parent = nullptr);

    // Call synchronously on foreground changes. Advance the epoch when provider,
    // account, device catalog or trust changes. Zero means fetching is disabled.
    quint64 setContext(quint64 epoch, bool foreground);

    // Accepts one fetch at a time. A nonzero request ID gets a finished signal.
    // Retain authorization across async route preparation, then pass it here.
    quint64 start(const Discovery::Device &device, const Discovery::NetworkSnapshot *network,
                  const FetchRequest &request);
    void cancel(quint64 request);

signals:
    void finished(quint64 request, Aurora::Wallpaper::FetchResult result);

private:
    struct Attempt;

    void launch(Attempt *attempt, Stream::Request request);
    bool collect(Attempt *attempt);
    void processFinished(Attempt *attempt, bool success);
    void complete(Attempt *attempt, FetchResult result);

    const QString dataDirectory_;
    const QString moonlightProgram_;
    Attempt *active_ = nullptr;
    quint64 epoch_ = 0;
    quint64 authorization_ = 0;
    quint64 nextRequest_ = 0;
    bool foreground_ = false;
};

} // namespace Aurora::Wallpaper

Q_DECLARE_METATYPE(Aurora::Wallpaper::FetchResult)

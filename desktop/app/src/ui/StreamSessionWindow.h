// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <QUrl>
#include <QUuid>

#include <memory>

class QQmlEngine;
class QWindow;

namespace Aurora::Session {
class SessionSupervisor;
struct SessionView;
}

// The Qt engine outlives the window. Only embedded playback prepares a window here.
class StreamSessionWindow final
{
public:
    StreamSessionWindow(QQmlEngine &engine, const QUrl &source);
    ~StreamSessionWindow();

    QString prepare(const QUuid &attempt, const QString &deviceName,
                    Aurora::Session::SessionSupervisor &session, Aurora::Session::SessionView &view);
    void show();
    void close(const QUuid &attempt);

private:
    QQmlEngine &engine_;
    const QUrl source_;
    std::unique_ptr<QWindow> window_;
    QUuid attempt_;
};

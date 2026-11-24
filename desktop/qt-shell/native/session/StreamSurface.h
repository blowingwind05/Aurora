// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QPointer>
#include <QProcess>
#include <QRect>
#include <QTimer>
#include <QWindow>

#include <memory>

namespace Aurora::Session {

class StreamSurface final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool videoReady READ videoReady NOTIFY stateChanged)
    Q_PROPERTY(bool attachmentFailed READ attachmentFailed NOTIFY stateChanged)
    Q_PROPERTY(bool surfaceVisible READ surfaceVisible NOTIFY surfaceVisibleChanged)
    Q_PROPERTY(QSize clientPixelSize READ clientPixelSize NOTIFY clientPixelSizeChanged)

public:
    enum class State { Idle, AwaitingWorker, Connecting, ConnectionStage, Attached, Stopping,
                       Finished, AttachmentFailed };
    Q_ENUM(State)

    explicit StreamSurface(QObject *parent = nullptr);
    ~StreamSurface() override;

    static bool available();
    // Call on the GUI thread. The returned invitation is scoped to this native surface.
    QString prepare(QWindow *owner, const QRect &viewport);
    // Bind the coordinator's actual QProcess before it starts.
    bool bindWorker(QProcess *worker);
    bool handleWorkerLine(const QProcess *source, const QByteArray &line);

    State state() const { return state_; }
    bool videoReady() const { return videoReady_; }
    bool attachmentFailed() const { return state_ == State::AttachmentFailed; }
    bool surfaceVisible() const { return surfaceVisible_; }
    QSize clientPixelSize() const { return clientPixelSize_; }

    // The rectangle uses coordinates in the owner's client area, in Qt logical pixels.
    Q_INVOKABLE void setViewport(const QRect &viewport, bool suppressed);
    Q_INVOKABLE void requestStop();

signals:
    void stateChanged();
    void fullscreenRequested();
    void surfaceVisibleChanged();
    void clientPixelSizeChanged(const QSize &size);
    void workerFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void surfaceLost();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Native;
    void workerStarted();
    void poll();
    void updateViewport();
    void loseSurface();
    void finish(State state);
    void release();
    void setState(State state);

    std::unique_ptr<Native> native_;
    QPointer<QWindow> owner_;
    QPointer<QWindow> host_;
    QPointer<QProcess> worker_;
    QMetaObject::Connection workerConnections_[4];
    QTimer timer_{this};
    QRect viewport_;
    QSize clientPixelSize_;
    State state_ = State::Idle;
    bool suppressed_ = false;
    bool stageReported_ = false;
    bool videoReady_ = false;
    bool surfaceVisible_ = false;
};

} // namespace Aurora::Session

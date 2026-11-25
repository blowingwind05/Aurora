// SPDX-License-Identifier: GPL-3.0-only
#include "StreamSurface.h"

#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QThread>
#include <QUuid>

#ifdef Q_OS_WIN
#include <windows.h>

#include <cwchar>
#include <limits>
#endif

namespace Aurora::Session {

#ifdef Q_OS_WIN
namespace {

constexpr wchar_t videoClass[] = L"AuroraEmbeddedStreamSurface";
constexpr int attachmentFailedExitCode = 74;

LPCWSTR wide(const QString &value)
{
    return reinterpret_cast<LPCWSTR>(value.utf16());
}

bool ownedBy(HWND window, DWORD expectedPid, bool child)
{
    DWORD pid = 0;
    return window && GetWindowThreadProcessId(window, &pid) && pid == expectedPid
        && ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0) == child;
}

HANDLE createNewEvent(const QString &name, bool manualReset)
{
    SetLastError(ERROR_SUCCESS);
    const HANDLE handle = CreateEventW(nullptr, manualReset, FALSE, wide(name));
    if (handle && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return nullptr;
    }
    return handle;
}

struct ChildSearch
{
    HWND host;
    DWORD pid;
    bool renderer;
    bool found = false;
};

BOOL CALLBACK searchChild(HWND child, LPARAM context)
{
    auto &search = *reinterpret_cast<ChildSearch *>(context);
    if (GetParent(child) != search.host || !ownedBy(child, search.pid, true)
        || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) {
        return TRUE;
    }

    wchar_t name[128]{};
    if (GetClassNameW(child, name, 128) && (std::wcscmp(name, videoClass) == 0) == search.renderer) {
        search.found = true;
        return FALSE;
    }
    return TRUE;
}

} // namespace
#endif

struct StreamSurface::Native
{
#ifdef Q_OS_WIN
    ~Native()
    {
        clearHost();
        for (HANDLE handle : {ready, stop, fullscreen, process}) {
            if (handle)
                CloseHandle(handle);
        }
    }

    void clearHost()
    {
        if (host && GetPropW(host, wide(tag)) == reinterpret_cast<HANDLE>(1))
            RemovePropW(host, wide(tag));
        host = nullptr;
    }

    bool hasHost() const
    {
        const DWORD pid = GetCurrentProcessId();
        return ownedBy(owner, pid, false) && ownedBy(host, pid, true) && GetParent(host) == owner
            && GetPropW(host, wide(tag)) == reinterpret_cast<HANDLE>(1);
    }

    bool liveWorker() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }

    bool hasChild(bool renderer) const
    {
        if (!hasHost() || !liveWorker())
            return false;
        ChildSearch search{host, workerPid, renderer};
        EnumChildWindows(host, searchChild, reinterpret_cast<LPARAM>(&search));
        return search.found;
    }

    QString tag;
    HWND owner = nullptr;
    HWND host = nullptr;
    HANDLE ready = nullptr;
    HANDLE stop = nullptr;
    HANDLE fullscreen = nullptr;
    HANDLE process = nullptr;
    DWORD workerPid = 0;
#endif
};

StreamSurface::StreamSurface(QObject *parent) : QObject(parent)
{
    timer_.setInterval(80);
    connect(&timer_, &QTimer::timeout, this, &StreamSurface::poll);
}

StreamSurface::~StreamSurface()
{
    release();
}

bool StreamSurface::available()
{
#ifdef Q_OS_WIN
    return qGuiApp && QGuiApplication::platformName() == QStringLiteral("windows");
#else
    return false;
#endif
}

QString StreamSurface::prepare(QWindow *owner, const QRect &viewport)
{
#ifdef Q_OS_WIN
    if (native_ || worker_)
        return {};
    if (!available() || !owner || QThread::currentThread() != thread() || owner->thread() != thread()
        || !owner->isTopLevel()) {
        setState(State::AttachmentFailed);
        return {};
    }

    const DWORD pid = GetCurrentProcessId();
    const auto ownerHandle = reinterpret_cast<HWND>(owner->winId());
    if (!ownedBy(ownerHandle, pid, false)) {
        setState(State::AttachmentFailed);
        return {};
    }

    auto native = std::make_unique<Native>();
    native->owner = ownerHandle;
    const auto id = QUuid::createUuid().toString(QUuid::Id128);
    native->tag = QStringLiteral("Aurora.Embedded.") + id;
    const auto eventPrefix = QStringLiteral("Local\\") + native->tag;
    native->ready = createNewEvent(eventPrefix + QStringLiteral(".ready"), true);
    native->stop = createNewEvent(eventPrefix + QStringLiteral(".stop"), true);
    native->fullscreen = createNewEvent(eventPrefix + QStringLiteral(".fullscreen"), false);
    if (!native->ready || !native->stop || !native->fullscreen) {
        setState(State::AttachmentFailed);
        return {};
    }

    auto host = std::make_unique<QWindow>(owner);
    host->setFlags(Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    host->setGeometry(0, 0, 1, 1);
    const auto hwnd = reinterpret_cast<HWND>(host->winId());
    if (!ownedBy(hwnd, pid, true) || GetParent(hwnd) != ownerHandle
        || !SetPropW(hwnd, wide(native->tag), reinterpret_cast<HANDLE>(1))) {
        setState(State::AttachmentFailed);
        return {};
    }
    native->host = hwnd;
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);

    owner_ = owner;
    host_ = host.release();
    native_ = std::move(native);
    owner_->installEventFilter(this);
    host_->installEventFilter(this);
    viewport_ = viewport;
    suppressed_ = false;
    timer_.start();
    setState(State::AwaitingWorker);
    updateViewport();
    if (!native_ || native_->host != hwnd || state_ != State::AwaitingWorker)
        return {};
    return QStringLiteral("%1:%2:%3").arg(static_cast<qint64>(reinterpret_cast<qintptr>(hwnd)))
                                     .arg(pid).arg(id);
#else
    Q_UNUSED(owner);
    Q_UNUSED(viewport);
    setState(State::AttachmentFailed);
    return {};
#endif
}

bool StreamSurface::bindWorker(QProcess *worker)
{
    if (state_ != State::AwaitingWorker || !worker || worker_ || worker->thread() != thread()
        || worker->state() != QProcess::NotRunning) {
        return false;
    }
    worker_ = worker;
    workerConnections_[0] = connect(worker, &QProcess::started, this, &StreamSurface::workerStarted);
    workerConnections_[1] = connect(worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(State::AttachmentFailed);
    });
    workerConnections_[2] = connect(worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
#ifdef Q_OS_WIN
        const bool attachmentFailed = status == QProcess::NormalExit && code == attachmentFailedExitCode && !videoReady_;
#else
        const bool attachmentFailed = false;
#endif
        finish(attachmentFailed || state_ == State::AttachmentFailed ? State::AttachmentFailed : State::Finished);
        emit workerFinished(code, status);
    });
    workerConnections_[3] = connect(worker, &QObject::destroyed, this, [this] { finish(State::Finished); });
    return true;
}

void StreamSurface::workerStarted()
{
#ifdef Q_OS_WIN
    const auto pid = worker_->processId();
    if (pid > 0 && pid <= (std::numeric_limits<DWORD>::max)()) {
        native_->workerPid = static_cast<DWORD>(pid);
        native_->process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, native_->workerPid);
    }
    if (!native_->process || GetProcessId(native_->process) != native_->workerPid) {
        SetEvent(native_->stop);
        setState(State::AttachmentFailed);
        return;
    }
    if (state_ == State::AwaitingWorker)
        setState(State::Connecting);
    poll();
#endif
}

bool StreamSurface::handleWorkerLine(const QProcess *source, const QByteArray &line)
{
    if (!worker_ || source != worker_ || (line != "aurora-connection-stage/1,qt"
        && line != "aurora-connection-stage/1,qt\n" && line != "aurora-connection-stage/1,qt\r\n")) {
        return false;
    }
    stageReported_ = true;
    poll();
    return true;
}

void StreamSurface::poll()
{
#ifdef Q_OS_WIN
    if (!native_ || !native_->host)
        return;
    if (!native_->hasHost()) {
        loseSurface();
        return;
    }

    if (state_ == State::Connecting || state_ == State::ConnectionStage) {
        if (WaitForSingleObject(native_->ready, 0) == WAIT_OBJECT_0 && native_->hasChild(true)) {
            videoReady_ = true;
            setState(State::Attached);
        } else if (state_ == State::Connecting && stageReported_ && native_->hasChild(false)) {
            setState(State::ConnectionStage);
        }
    }
    if (!native_)
        return;
    if (state_ == State::Attached && native_->liveWorker()
        && WaitForSingleObject(native_->fullscreen, 0) == WAIT_OBJECT_0) {
        emit fullscreenRequested();
    }
    updateViewport();
#endif
}

void StreamSurface::setViewport(const QRect &viewport, bool suppressed)
{
    viewport_ = viewport;
    suppressed_ = suppressed;
    updateViewport();
}

void StreamSurface::updateViewport()
{
#ifdef Q_OS_WIN
    if (!host_ || !owner_ || !native_ || !native_->host)
        return;
    const auto bounds = viewport_.intersected(QRect(QPoint(), owner_->size()));
    if (!bounds.isEmpty() && owner_->windowState() != Qt::WindowMinimized && host_->geometry() != bounds)
        host_->setGeometry(bounds);

    const bool visible = !bounds.isEmpty() && !suppressed_ && owner_->isVisible()
        && owner_->windowState() != Qt::WindowMinimized
        && (state_ == State::Attached || state_ == State::ConnectionStage
            || (state_ == State::Stopping && surfaceVisible_));
    if (host_->isVisible() != visible)
        host_->setVisible(visible);
    if (surfaceVisible_ != visible) {
        surfaceVisible_ = visible;
        emit surfaceVisibleChanged();
    }

    RECT pixels{};
    const QSize size = !bounds.isEmpty() && native_ && native_->host && GetClientRect(native_->host, &pixels)
        ? QSize(pixels.right, pixels.bottom) : QSize();
    if (size != clientPixelSize_) {
        clientPixelSize_ = size;
        emit clientPixelSizeChanged(size);
    }
#endif
}

void StreamSurface::requestStop()
{
#ifdef Q_OS_WIN
    if (!native_ || state_ == State::Stopping || state_ == State::AttachmentFailed)
        return;
    SetEvent(native_->stop);
    if (worker_ && worker_->state() != QProcess::NotRunning)
        setState(State::Stopping);
    else
        finish(State::Finished);
#endif
}

bool StreamSurface::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == owner_ || watched == host_) {
        if (event->type() == QEvent::PlatformSurface
            && static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            loseSurface();
        } else if (watched == owner_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show
                   || event->type() == QEvent::Hide || event->type() == QEvent::WindowStateChange)) {
            // Read the new Qt geometry and visibility after QWindow processes the event.
            QTimer::singleShot(0, this, &StreamSurface::updateViewport);
        }
    }
    return QObject::eventFilter(watched, event);
}

void StreamSurface::loseSurface()
{
#ifdef Q_OS_WIN
    if (!native_ || !native_->host)
        return;
    SetEvent(native_->stop);
    native_->clearHost();
    timer_.stop();
    const bool wasVisible = surfaceVisible_;
    surfaceVisible_ = false;
    if (wasVisible)
        emit surfaceVisibleChanged();
    if (clientPixelSize_.isValid()) {
        clientPixelSize_ = QSize();
        emit clientPixelSizeChanged(clientPixelSize_);
    }
    setState(State::Stopping);
    // Native destruction can be in progress in the caller's stack.
    QTimer::singleShot(0, this, [this] {
        if (!native_ || native_->host)
            return;
        if (host_ && host_->handle())
            host_->hide();
        if (state_ == State::Stopping && (!worker_ || worker_->state() == QProcess::NotRunning))
            finish(State::Finished);
    });
    emit surfaceLost();
#endif
}

void StreamSurface::finish(State state)
{
    const bool wasVisible = surfaceVisible_;
    const bool hadSize = clientPixelSize_.isValid();
    release();
    setState(state);
    if (wasVisible)
        emit surfaceVisibleChanged();
    if (hadSize)
        emit clientPixelSizeChanged(clientPixelSize_);
}

void StreamSurface::release()
{
    timer_.stop();
    for (auto &connection : workerConnections_) {
        disconnect(connection);
        connection = {};
    }
    worker_.clear();
    if (owner_)
        owner_->removeEventFilter(this);
    owner_.clear();
#ifdef Q_OS_WIN
    if (native_ && native_->stop) {
        SetEvent(native_->stop);
        native_->clearHost();
    }
#endif
    if (host_) {
        host_->removeEventFilter(this);
        delete host_;
    }
    native_.reset();
    clientPixelSize_ = QSize();
    surfaceVisible_ = false;
    stageReported_ = false;
    videoReady_ = false;
}

void StreamSurface::setState(State state)
{
    if (state_ == state)
        return;
    state_ = state;
    emit stateChanged();
}

} // namespace Aurora::Session

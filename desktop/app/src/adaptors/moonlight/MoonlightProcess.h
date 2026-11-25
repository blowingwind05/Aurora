// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "MoonlightCommand.h"

#include <QProcess>

#include <memory>

namespace Aurora::Stream {

// A single child on the caller's Qt event-loop thread. Use QProcess signals
// and readAllStandardOutput() to consume the child's original protocol.
class MoonlightProcess final : public QProcess
{
public:
    explicit MoonlightProcess(QObject *parent = nullptr);
    ~MoonlightProcess() override;

    // A true return means the launch was requested. Observe started(),
    // errorOccurred() and finished() for the asynchronous result.
    bool launch(const Command &command, QString *error = nullptr);
    void cancel();

private:
    using QProcess::start;
    using QProcess::startDetached;

#ifdef Q_OS_WIN
    struct WindowsJob;
    std::unique_ptr<WindowsJob> job_;
#endif
    bool launchRequested_ = false;
};

} // namespace Aurora::Stream

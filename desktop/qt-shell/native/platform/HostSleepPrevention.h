// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

namespace Aurora::Platform {

enum class HostSleepMode { System, OnAc, Always };

std::optional<HostSleepMode> parseHostSleepMode(const QString &token);
QString hostSleepModeToken(HostSleepMode mode);

class HostSleepBackend;

// Own on one Qt event-loop thread for the lifetime of the desktop controller.
class HostSleepPrevention final : public QObject
{
public:
    explicit HostSleepPrevention(HostSleepMode initialMode = HostSleepMode::System);
    HostSleepPrevention(HostSleepMode initialMode, std::unique_ptr<HostSleepBackend> backend);
    ~HostSleepPrevention() override;

    HostSleepMode mode() const { return mode_; }
    // Apply the preference after its owner successfully persists it.
    void applyMode(HostSleepMode mode);
    void setHostActive(bool active);

private:
    void update();
    void refreshAssertion();
    void releaseAssertion();

    std::unique_ptr<HostSleepBackend> backend_;
    QTimer refreshTimer_{this};
    HostSleepMode mode_;
    bool hostActive_ = false;
    bool resumeMonitoring_ = false;
    bool assertionHeld_ = false;
};

} // namespace Aurora::Platform

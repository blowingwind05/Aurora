// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BundledAdmin.h"
#include "SunshineSettings.h"

namespace Aurora::Sunshine {

class ReadinessAdmin;

SettingsSaveResult rejectedSettingsEdit(const SettingsEdit &edit, SettingsStatus status, const QString &error);
QString staleSettingsMessage();
QString cancelledSettingsMessage();

// Owned by the lifecycle worker for one pinned, authenticated launch.
class SettingsSession final : public QObject
{
public:
    using ReadCompletion = std::function<void(SettingsReadResult, BundledAdmin::Error)>;
    using SaveCompletion = std::function<void(SettingsSaveResult, BundledAdmin::Error)>;

    SettingsSession(QString session, ReadinessAdmin &admin, std::function<bool()> isCurrent);

    const QString &id() const { return session_; }
    void read(ReadCompletion done);
    void save(const SettingsEdit &edit, SaveCompletion done);

private:
    SettingsSnapshot publish(QJsonObject config);

    const QString session_;
    ReadinessAdmin &admin_;
    const std::function<bool()> isCurrent_;
    quint64 revision_ = 0;
    std::optional<QJsonObject> latestConfig_;
};

} // namespace Aurora::Sunshine

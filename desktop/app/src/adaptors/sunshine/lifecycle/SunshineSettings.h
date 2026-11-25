// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "SunshineConfig.h"

#include <QMetaType>
#include <QStringList>

#include <functional>
#include <optional>

namespace Aurora::Sunshine {

struct SettingsVersion
{
    QString session;
    quint64 revision = 0;
};

struct SettingsSnapshot
{
    SettingsVersion version;
    QJsonObject config;
};

enum class SettingsStatus { Complete, Partial, Rejected, Stale, Unavailable, TransportError, Cancelled };

struct SettingsReadResult
{
    QString session;
    SettingsStatus status = SettingsStatus::Unavailable;
    QString error;
    std::optional<SettingsSnapshot> snapshot;
};

struct SettingsEdit
{
    SettingsVersion version;
    int id = 0;
    QJsonObject changes;
};

struct SettingsSaveResult
{
    SettingsVersion submitted;
    int id = 0;
    SettingsStatus status = SettingsStatus::Unavailable;
    QString error;
    QStringList saved;
    Values errors;
    std::optional<SettingsSnapshot> snapshot;
    bool refreshRequired = false;

    QJsonObject receiptForQml() const;
};

using SettingsReadCompletion = std::function<void(SettingsReadResult)>;
using SettingsSaveCompletion = std::function<void(SettingsSaveResult)>;

} // namespace Aurora::Sunshine

Q_DECLARE_METATYPE(Aurora::Sunshine::SettingsReadResult)
Q_DECLARE_METATYPE(Aurora::Sunshine::SettingsSaveResult)

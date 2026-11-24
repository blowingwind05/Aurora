// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QMap>
#include <QString>

namespace Aurora::Preferences {

// Absolute path to the Aurora directory shared with the installed desktop app.
QString defaultDirectory();

enum class SleepMode { System, OnAc, Always };
enum class StartupClaim { Claimed, AlreadyInitialized, Failed };

class GeneralPreferences
{
public:
    explicit GeneralPreferences(const QString &directory = defaultDirectory());

    bool closeToTray() const;
    bool setCloseToTray(bool enabled) const;

    SleepMode sleepMode() const;
    bool setSleepMode(SleepMode mode) const;

    bool managedHostAllowed() const;
    bool setManagedHostAllowed(bool allowed) const;

    bool startupInitialized() const;
    StartupClaim claimStartupInitialization() const;

private:
    QString directory_;
};

struct StreamPreferenceSnapshot
{
    QMap<QString, QString> overrides;

    // Returns the effective value, or an empty string for an unknown option.
    QString value(const QString &option) const;
};

class StreamPreferences
{
public:
    explicit StreamPreferences(const QString &directory = defaultDirectory());

    StreamPreferenceSnapshot read() const;
    bool set(const QString &option, const QString &value) const;
    bool reset() const;

private:
    QString path_;
};

} // namespace Aurora::Preferences

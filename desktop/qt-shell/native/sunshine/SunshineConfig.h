// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

#include <optional>

namespace Aurora::Sunshine {

using Values = QMap<QString, QString>;

struct Preferences
{
    QString name;
    bool streamAudio = true;
};

struct DisplaySelection
{
    QString id;
    bool exclusive = false;
    bool screensaver = false;
};

struct Display
{
    QString id;
    QString name;
};

struct PrivacySettings
{
    DisplaySelection saved;
    DisplaySelection active;
    bool supportsVirtualDisplay = false;
    bool supportsExtended = false;
    bool supportsScreensaver = false;
    QList<Display> displays;
};

class ValidatedChanges;

class PreferenceUpdate
{
public:
    const Preferences &preferences() const { return preferences_; }
    const std::optional<DisplaySelection> &display() const { return display_; }

private:
    friend std::optional<PreferenceUpdate> resolvePreferences(const ValidatedChanges &changes,
        const QJsonObject &current, const std::optional<PrivacySettings> &privacy, QString &error);

    PreferenceUpdate() = default;
    Preferences preferences_;
    std::optional<DisplaySelection> display_;
};

class ValidatedChanges
{
public:
    const Values &preferences() const { return preferences_; }
    const Values &options() const { return options_; }
    const Values &errors() const { return errors_; }

private:
    friend ValidatedChanges validateChanges(const QJsonObject &changes, bool managed);
    friend std::optional<PreferenceUpdate> resolvePreferences(const ValidatedChanges &changes,
        const QJsonObject &current, const std::optional<PrivacySettings> &privacy, QString &error);

    Values preferences_;
    Values options_;
    Values errors_;
    bool managed_ = false;
};

const Values &managedOptions();
Preferences parsePreferences(const QJsonObject &current);
PrivacySettings parsePrivacySettings(const QJsonObject &current);

// Returns the legacy state.config object, with ordered options and a nullable privacy object.
QJsonObject configForQml(const QJsonObject &current, bool managed,
                         const std::optional<PrivacySettings> &privacy = std::nullopt);

// Only accepted values appear in either write group. Every rejected field has an error.
ValidatedChanges validateChanges(const QJsonObject &changes, bool managed);

// Skip the corresponding write when its group is empty.
QJsonObject mergeOptions(const QJsonObject &current, const ValidatedChanges &changes);
std::optional<PreferenceUpdate> resolvePreferences(const ValidatedChanges &changes, const QJsonObject &current,
    const std::optional<PrivacySettings> &privacy, QString &error);
std::optional<QJsonObject> mergePreferences(const QJsonObject &fresh, const PreferenceUpdate &update,
    const std::optional<PrivacySettings> &freshPrivacy, QString &error);

// An empty error confirms the requested values.
QString verifyOptions(const QJsonObject &actual, const ValidatedChanges &changes);
QString verifyPreferences(const QJsonObject &actual, const PreferenceUpdate &update,
                          const std::optional<PrivacySettings> &privacy = std::nullopt);

} // namespace Aurora::Sunshine

// SPDX-License-Identifier: GPL-3.0-only
#include "SettingsSession.h"
#include "LifecycleController.h"

#include <QJsonArray>
#include <QPointer>

#include <algorithm>
#include <memory>
#include <utility>

namespace Aurora::Sunshine {
namespace {

using AdminError = BundledAdmin::Error;

QString unavailableMessage()
{
    return QStringLiteral("当前 Sunshine 管理会话无法处理设置请求。");
}

void recordGroup(SettingsSaveResult &result, const Values &values, const QString &error)
{
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (error.isEmpty())
            result.saved.append(it.key());
        else
            result.errors.insert(it.key(), error);
    }
}

bool changedSinceRead(const QJsonObject &original, const QJsonObject &fresh, const Values &changes)
{
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (original.value(it.key()) != fresh.value(it.key()))
            return true;
    }
    return false;
}

struct Write
{
    ValidatedChanges changes;
    SettingsSaveResult result;
    QJsonObject original;
    std::optional<PreferenceUpdate> preferences;
    std::optional<QJsonObject> observed;
    bool prepared = false;
    bool readBack = false;
};

} // namespace

QString staleSettingsMessage()
{
    return QStringLiteral("Sunshine 设置已变化，请重新读取并确认后再保存。");
}

QString cancelledSettingsMessage()
{
    return QStringLiteral("Sunshine 管理会话已结束，请重新读取设置。");
}

SettingsSaveResult rejectedSettingsEdit(const SettingsEdit &edit, SettingsStatus status, const QString &error)
{
    SettingsSaveResult result;
    result.submitted = edit.version;
    result.id = edit.id;
    result.status = status;
    result.error = error;
    for (auto it = edit.changes.constBegin(); it != edit.changes.constEnd(); ++it)
        result.errors.insert(it.key(), error);
    return result;
}

QJsonObject SettingsSaveResult::receiptForQml() const
{
    QJsonObject fields;
    for (auto it = errors.cbegin(); it != errors.cend(); ++it)
        fields.insert(it.key(), it.value());
    return {{QStringLiteral("id"), id},
            {QStringLiteral("saved"), QJsonArray::fromStringList(saved)},
            {QStringLiteral("errors"), fields}};
}

SettingsSession::SettingsSession(QString session, ReadinessAdmin &admin, std::function<bool()> isCurrent)
    : session_(std::move(session)), admin_(admin), isCurrent_(std::move(isCurrent))
{
}

SettingsSnapshot SettingsSession::publish(QJsonObject config)
{
    latestConfig_ = std::move(config);
    return {{session_, ++revision_}, configForQml(*latestConfig_, true)};
}

void SettingsSession::read(ReadCompletion done)
{
    const QPointer<SettingsSession> self(this);
    const auto started = admin_.readConfig([self, done](BundledAdmin::Result reply) {
        if (!self || !self->isCurrent_())
            return;
        if (reply) {
            auto view = self->publish(std::move(reply.config));
            done({self->session_, SettingsStatus::Complete, {}, std::move(view)}, AdminError::None);
        } else {
            done({self->session_, SettingsStatus::TransportError, std::move(reply.message), std::nullopt}, reply.error);
        }
    });
    if (!started)
        done({session_, SettingsStatus::Unavailable, unavailableMessage(), std::nullopt}, AdminError::None);
}

void SettingsSession::save(const SettingsEdit &edit, SaveCompletion done)
{
    if (edit.version.session != session_ || !latestConfig_ || edit.version.revision != revision_) {
        auto result = rejectedSettingsEdit(edit, SettingsStatus::Stale, staleSettingsMessage());
        result.refreshRequired = edit.version.session == session_ && !latestConfig_;
        done(std::move(result), AdminError::None);
        return;
    }
    if (edit.id <= 0 || edit.changes.isEmpty() || edit.changes.size() > 110) {
        done(rejectedSettingsEdit(edit, SettingsStatus::Rejected, QStringLiteral("无效的 Sunshine 设置请求。")), AdminError::None);
        return;
    }

    auto work = std::make_shared<Write>();
    work->changes = validateChanges(edit.changes, true);
    work->result.submitted = edit.version;
    work->result.id = edit.id;
    work->result.status = SettingsStatus::Rejected;
    work->result.errors = work->changes.errors();
    if (work->changes.options().isEmpty() && work->changes.preferences().isEmpty()) {
        done(std::move(work->result), AdminError::None);
        return;
    }
    work->original = *latestConfig_;

    const QPointer<SettingsSession> self(this);
    const auto current = [self, version = edit.version] {
        return self && self->isCurrent_() && self->session_ == version.session && self->revision_ == version.revision;
    };
    const auto prepare = [work, current](const QJsonObject &fresh, QString &error) -> std::optional<QJsonObject> {
        if (!current()) {
            error = cancelledSettingsMessage();
            return std::nullopt;
        }

        const auto &options = work->changes.options();
        const auto &preferences = work->changes.preferences();
        if (changedSinceRead(work->original, fresh, options) || changedSinceRead(work->original, fresh, preferences)) {
            error = staleSettingsMessage();
            work->result.status = SettingsStatus::Stale;
            work->result.error = error;
            recordGroup(work->result, options, error);
            recordGroup(work->result, preferences, error);
            work->observed = fresh;
            return std::nullopt;
        }

        auto payload = mergeOptions(fresh, work->changes);
        if (!preferences.isEmpty()) {
            work->preferences = resolvePreferences(work->changes, fresh, std::nullopt, error);
            if (work->preferences) {
                auto merged = mergePreferences(payload, *work->preferences, std::nullopt, error);
                if (merged)
                    payload = std::move(*merged);
                else
                    work->preferences.reset();
            }
            if (!work->preferences)
                recordGroup(work->result, preferences, error);
        }

        if (!work->preferences && options.isEmpty()) {
            work->observed = fresh;
            return std::nullopt;
        }
        work->prepared = true;
        return payload;
    };
    const auto verify = [work, current](const QJsonObject &actual) {
        if (!current())
            return cancelledSettingsMessage();

        work->readBack = true;
        work->observed = actual;
        const auto optionError = verifyOptions(actual, work->changes);
        recordGroup(work->result, work->changes.options(), optionError);

        QString preferenceError;
        if (work->preferences) {
            preferenceError = verifyPreferences(actual, *work->preferences);
            recordGroup(work->result, work->changes.preferences(), preferenceError);
        }
        return optionError.isEmpty() ? preferenceError : optionError;
    };

    const auto started = admin_.saveConfig(prepare, verify, [self, work, current, done](BundledAdmin::Result reply) {
        if (!current())
            return;
        auto &result = work->result;
        if (work->readBack && (reply || reply.error == AdminError::ReadbackMismatch)) {
            if (result.errors.isEmpty())
                result.status = SettingsStatus::Complete;
            else if (!result.saved.isEmpty())
                result.status = SettingsStatus::Partial;
            std::sort(result.saved.begin(), result.saved.end());
        } else if (!work->observed || work->prepared) {
            result.status = SettingsStatus::TransportError;
            result.error = reply.message.isEmpty() ? unavailableMessage() : std::move(reply.message);
            result.saved.clear();
            recordGroup(result, work->changes.options(), result.error);
            if (work->preferences || !work->prepared)
                recordGroup(result, work->changes.preferences(), result.error);
            if (work->prepared) {
                work->observed.reset();
                self->latestConfig_.reset();
                result.refreshRequired = true;
            }
        }
        if (work->observed)
            result.snapshot = self->publish(std::move(*work->observed));
        done(std::move(result), reply.error);
    });
    if (!started)
        done(rejectedSettingsEdit(edit, SettingsStatus::Unavailable, unavailableMessage()), AdminError::None);
}

} // namespace Aurora::Sunshine

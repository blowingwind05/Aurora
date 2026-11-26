// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDeadlineTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVariantMap>

#include <limits>
#include <optional>

namespace AuroraDisplayControls {

inline std::optional<quint32> frameNumber(const QJsonValue& value)
{
    const double number = value.toDouble(-1);
    if (!(number >= 0 && number <= std::numeric_limits<quint32>::max()))
        return {};
    const auto integer = static_cast<quint32>(number);
    return number == integer ? std::optional<quint32>(integer) : std::nullopt;
}

inline QString displayId(const QJsonValue& value)
{
    const double number = value.toDouble();
    if (number >= 1 && number <= std::numeric_limits<quint32>::max() &&
        number == static_cast<quint32>(number))
        return QString::number(static_cast<quint32>(number));
    const auto id = value.toString();
    return id.toUInt() && id == QString::number(id.toUInt()) ? id : QString();
}

inline QVariantMap snapshot(const QByteArray& response)
{
    if (response.isEmpty() || response.size() > 8192)
        return {};
    const auto object = QJsonDocument::fromJson(response).object();
    const auto status = object.value(QStringLiteral("status")).toString();
    const auto outputs = object.value(QStringLiteral("displays")).toArray();
    const auto selected = displayId(object.value(QStringLiteral("activeDisplayId")));
    if (status.isEmpty() || selected.isEmpty() || outputs.isEmpty() || outputs.size() > 32 ||
        !object.value(QStringLiteral("canCreate")).isBool())
        return {};

    QVariantList displays;
    QSet<QString> seen;
    for (const auto& entry : outputs) {
        const auto display = entry.toObject();
        const auto id = displayId(display.value(QStringLiteral("id")));
        const auto kind = display.value(QStringLiteral("kind")).toString();
        const auto ready = display.value(QStringLiteral("ready"));
        const auto removable = display.value(QStringLiteral("removable"));
        const auto name = display.value(QStringLiteral("name")).toString().trimmed();
        if (id.isEmpty() || seen.contains(id) || !ready.isBool() || !removable.isBool() ||
            name.isEmpty() || name.size() > 128 || (kind != QStringLiteral("physical") && kind != QStringLiteral("virtual")) ||
            (id == selected && !ready.toBool()))
            return {};
        seen.insert(id);

        QVariantMap item{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("ready"), ready.toBool()},
            {QStringLiteral("selectable"), ready.toBool() &&
                display.value(QStringLiteral("selectable")).toBool()},
            {QStringLiteral("aspectBlocked"), display.value(QStringLiteral("selectBlockReason")) ==
                QStringLiteral("incompatible-aspect")},
            {QStringLiteral("removable"), kind == QStringLiteral("virtual") && removable.toBool()},
        };
        const auto width = display.value(QStringLiteral("width")).toInt();
        const auto height = display.value(QStringLiteral("height")).toInt();
        const auto rate = display.value(QStringLiteral("refreshRate")).toDouble();
        if (width > 0 && height > 0)
            item.insert(QStringLiteral("resolution"), QStringLiteral("%1 × %2").arg(width).arg(height));
        if (rate > 0 && rate <= 1000)
            item.insert(QStringLiteral("refreshRate"), QStringLiteral("%1 Hz").arg(rate));
        displays.push_back(item);
    }
    if (!seen.contains(selected))
        return {};
    QVariantMap result{{QStringLiteral("status"), status},
                       {QStringLiteral("displays"), displays},
                       {QStringLiteral("activeDisplayId"), selected},
                       {QStringLiteral("canSelect"), object.value(QStringLiteral("canSelect")).toBool()},
                       {QStringLiteral("canCreate"), object.value(QStringLiteral("canCreate")).toBool()}};
    const auto pending = object.value(QStringLiteral("pendingDisplayId"));
    if (!pending.isUndefined() && !pending.isNull()) {
        const auto pendingId = displayId(pending);
        const auto generation = frameNumber(object.value(QStringLiteral("switchGeneration")));
        if (pendingId == selected || !seen.contains(pendingId) || !generation || !*generation)
            return {};
        result.insert(QStringLiteral("pendingDisplayId"), pendingId);
        result.insert(QStringLiteral("switchGeneration"), *generation);
        const auto firstFrame = object.value(QStringLiteral("firstFrame"));
        if (!firstFrame.isUndefined() && !firstFrame.isNull()) {
            const auto frame = frameNumber(firstFrame);
            if (!frame)
                return {};
            result.insert(QStringLiteral("firstFrame"), *frame);
        }
    }
    return result;
}

enum class SwitchProgress { Poll, Present, Applied, Rejected, Uncertain };

class DisplaySwitch {
public:
    void begin(const QString& target)
    {
        target_ = target;
        deadline_ = QDeadlineTimer(18000);
        generation_ = 0;
        presentationRequested_ = false;
    }

    void clear() { target_.clear(); }
    bool active() const { return !target_.isEmpty(); }
    bool expired() const { return deadline_.hasExpired(); }
    const QString& target() const { return target_; }
    quint32 generation() const { return generation_; }
    quint32 firstFrame() const { return firstFrame_; }
    void presentationRequested() { presentationRequested_ = true; }

    SwitchProgress observe(const QVariantMap& state)
    {
        if (state.isEmpty())
            return SwitchProgress::Poll;
        const auto pending = state.value(QStringLiteral("pendingDisplayId")).toString();
        if (pending.isEmpty()) {
            if (state.value(QStringLiteral("activeDisplayId")).toString() != target_)
                return SwitchProgress::Rejected;
            return presentationRequested_ ? SwitchProgress::Applied : SwitchProgress::Uncertain;
        }
        const auto generation = state.value(QStringLiteral("switchGeneration")).toUInt();
        if (pending != target_ || !generation || (generation_ && generation != generation_))
            return SwitchProgress::Uncertain;
        generation_ = generation;
        if (!state.contains(QStringLiteral("firstFrame")) || presentationRequested_)
            return SwitchProgress::Poll;
        firstFrame_ = state.value(QStringLiteral("firstFrame")).toUInt();
        return SwitchProgress::Present;
    }

private:
    QString target_;
    QDeadlineTimer deadline_;
    quint32 generation_ = 0;
    quint32 firstFrame_ = 0;
    bool presentationRequested_ = false;
};

}  // namespace AuroraDisplayControls

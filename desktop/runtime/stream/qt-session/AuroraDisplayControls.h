// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVariantMap>

#include <limits>

namespace AuroraDisplayControls {

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
    return {{QStringLiteral("status"), status},
            {QStringLiteral("displays"), displays},
            {QStringLiteral("activeDisplayId"), selected},
            {QStringLiteral("canSelect"), object.value(QStringLiteral("canSelect")).toBool()},
            {QStringLiteral("canCreate"), object.value(QStringLiteral("canCreate")).toBool()}};
}
}  // namespace AuroraDisplayControls

// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BundledAdmin.h"

#include <QRandomGenerator>

#include <algorithm>
#include <cstring>

namespace Aurora::Pairing::HostOffer {

using Aurora::Sunshine::PendingPairing;

template <typename SourceMatches>
std::optional<PendingPairing> singlePending(const QList<PendingPairing> &rows,
    const QByteArray &marker, SourceMatches sourceMatches)
{
    const auto name = QStringLiteral("aurora-") + QString::fromLatin1(marker);
    const PendingPairing *match = nullptr;
    for (const auto &row : rows) {
        if (row.name != name || !sourceMatches(row))
            continue;
        if (match)
            return std::nullopt;
        match = &row;
    }
    // Sunshine approves by ID alone, so the chosen ID must identify only this row.
    if (!match || std::count_if(rows.cbegin(), rows.cend(),
            [match](const PendingPairing &row) { return row.id == match->id; }) != 1)
        return std::nullopt;
    return *match;
}

inline QByteArray frame(const QByteArray &marker, const QByteArray &pin)
{
    return QByteArray(1, '\1') + marker + pin;
}

inline bool samePending(const PendingPairing &a, const PendingPairing &b)
{
    return a.id == b.id && a.name == b.name && a.address == b.address &&
        a.sourceIpv4 == b.sourceIpv4 && a.sourcePort == b.sourcePort;
}

inline QString displayName(const QString &peerName)
{
    QString name;
    for (const auto ch : peerName) {
        if (ch != QLatin1Char('#') && ch.category() != QChar::Other_Control)
            name.append(ch);
        if (name.size() == 24)
            break;
    }
    if (!name.isEmpty() && name.at(name.size() - 1).isHighSurrogate())
        name.chop(1);
    return name.trimmed().isEmpty() ? QStringLiteral("Aurora") : name;
}

inline QByteArray randomBytes(int size)
{
    QByteArray bytes(size, '\0');
    for (int i = 0; i < size; i += int(sizeof(quint32))) {
        const auto word = QRandomGenerator::system()->generate();
        std::memcpy(bytes.data() + i, &word, size_t(std::min(size - i, int(sizeof(word)))));
    }
    return bytes;
}

inline QByteArray newPin()
{
    quint32 value;
    do {
        value = QRandomGenerator::system()->generate();
    } while (value >= 4294960000u);
    return QByteArray::number(value % 10000).rightJustified(4, '0');
}

} // namespace Aurora::Pairing::HostOffer

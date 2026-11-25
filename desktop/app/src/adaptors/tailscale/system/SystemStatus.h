// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../Status.h"

namespace aurora::tailscale::system {

struct Profile
{
    QString id;
    QString name;
    QString loginName;
    QString tailnet;
    bool isCurrent;
};

struct ProfilesResult
{
    StatusError error = StatusError::InvalidResponse;
    QVector<Profile> profiles;
};

constexpr int maxSystemStatusBytes = maxStatusReplyBytes - 64;
constexpr int maxProfilesBytes = 1024 * 1024;

StatusResult decodeSystemStatus(const QByteArray &bytes);
ProfilesResult decodeProfileList(const QByteArray &bytes);
ProfilesResult decodeLegacyProfileList(const QByteArray &bytes);
bool cliLacksJsonProfileList(const QByteArray &standardOutput, const QByteArray &standardError);

} // namespace aurora::tailscale::system

Q_DECLARE_METATYPE(aurora::tailscale::system::ProfilesResult)

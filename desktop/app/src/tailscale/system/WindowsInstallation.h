// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

#include <functional>

namespace aurora::tailscale::system {

enum class ServiceState { Unknown, Missing, Stopped, Starting, Running };
enum class ImagePathType { String, ExpandString };

struct ServiceImagePath
{
    QString text;
    ImagePathType type = ImagePathType::String;
};

struct Installation
{
    QString executable;
    ServiceState service = ServiceState::Missing;
};

// Pure identification from the registry value and the system Program Files path.
QString cliFromServiceImagePath(const ServiceImagePath &value, const QString &programFiles,
    const std::function<QString(const QString &)> &expand);

// Reads the registered service and queries its state without requesting control access.
Installation inspectWindowsInstallation();

} // namespace aurora::tailscale::system

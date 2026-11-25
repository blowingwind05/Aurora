// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "WindowsInstallation.h"

#include <QProcess>
#include <memory>
#include <optional>

class QTcpServer;

namespace Aurora::Pairing::SystemHostDetail {

QString runningProgram(const aurora::tailscale::system::Installation &installation);
bool prepareProcess(QProcess &process, const QString &program);
bool listenLoopback(QTcpServer &server);
bool tcpOwnedByProcess(quint32 pid, quint16 sourcePort, quint16 destinationPort);
std::optional<quint32> loopbackOwner(quint16 sourcePort, quint16 destinationPort);

// Attach the foreground relay to a kill-on-close job at process creation.
bool bindToParentJob(QProcess &process);

} // namespace Aurora::Pairing::SystemHostDetail

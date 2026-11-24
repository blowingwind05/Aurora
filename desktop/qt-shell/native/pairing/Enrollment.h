// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QString>

class QSettings;

namespace Aurora::Pairing {

enum class Provider { System, Embedded };
enum class EnrollmentResult { Approved, NeedsConfirmation, Unavailable };

struct ApprovalScope
{
    Provider provider;
    qint64 owner;
    QString localNode;
    quint16 adminPort;
    QByteArray certificateSha256;
};

QString legacyScope(const ApprovalScope &scope);
QString approvalId(const ApprovalScope &scope);

// Use an already authenticated account and the certificate from the managed admin session.
EnrollmentResult enrollLocalHost(const QString &dataDirectory, const ApprovalScope &scope, bool confirmed);

EnrollmentResult enrollRegistryHost(QSettings &settings, const ApprovalScope &scope, bool confirmed);

}

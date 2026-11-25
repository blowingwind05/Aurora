// SPDX-License-Identifier: GPL-3.0-only
#include "Enrollment.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

using namespace Aurora::Pairing;

int main(int argc, char **argv)
{
    const QCoreApplication application(argc, argv);
    const QTemporaryDir fixture;
    if (!fixture.isValid())
        return 1;
    auto first = ApprovalScope{Provider::Embedded, 42, QStringLiteral("local-node"), 48990, QByteArray(32, 'a')};
    auto next = first;
    ++next.owner;
    if (approvalId(first) != QStringLiteral("4941D178C0AB845A9E8C74CC8643C84402AFB219B64AD3F9285FDD17F88F8384"))
        return 2;

    QSettings settings(fixture.filePath(QStringLiteral("approvals.ini")), QSettings::IniFormat);
    if (enrollRegistryHost(settings, first, false) != EnrollmentResult::Approved ||
        enrollRegistryHost(settings, next, false) != EnrollmentResult::NeedsConfirmation ||
        enrollRegistryHost(settings, next, true) != EnrollmentResult::Approved)
        return 3;
    next.certificateSha256 = QByteArray(32, 'b');
    if (enrollRegistryHost(settings, next, false) != EnrollmentResult::NeedsConfirmation)
        return 4;

    QSettings previous(fixture.filePath(QStringLiteral("previous.ini")), QSettings::IniFormat);
    previous.setValue(QStringLiteral("Account"), legacyScope(first));
    if (enrollRegistryHost(previous, first, false) != EnrollmentResult::Approved ||
        enrollRegistryHost(previous, next, false) != EnrollmentResult::NeedsConfirmation)
        return 5;

#ifdef Q_OS_MACOS
    if (enrollLocalHost(fixture.path(), first, false) != EnrollmentResult::Approved ||
        enrollLocalHost(fixture.path(), next, false) != EnrollmentResult::NeedsConfirmation ||
        enrollLocalHost(fixture.path(), next, true) != EnrollmentResult::Approved)
        return 6;
#endif
    return 0;
}

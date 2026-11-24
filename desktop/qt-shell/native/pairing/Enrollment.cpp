// SPDX-License-Identifier: GPL-3.0-only
#include "Enrollment.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSettings>

#ifdef Q_OS_MACOS
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Aurora::Pairing {
namespace {
const auto accountValue = QStringLiteral("Account");
const auto approvalsGroup = QStringLiteral("ApprovedAccounts");

bool isApprovedDword(const QVariant &value)
{
    return (value.userType() == QMetaType::UInt || value.userType() == QMetaType::Int) && value.toUInt() == 1;
}

#ifdef Q_OS_MACOS
bool privateEntry(const QString &path, bool directory)
{
    struct stat info {};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0 || info.st_uid != ::geteuid())
        return false;
    const auto permission = directory ? 0700 : 0600;
    return (info.st_mode & 07777 & ~permission) == 0 &&
        (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_size == 0);
}

bool privateDirectory(const QString &path)
{
    ::mkdir(QFile::encodeName(path).constData(), 0700);
    return privateEntry(path, true);
}

EnrollmentResult enrollMacHost(const QString &root, const QString &id, bool confirmed)
{
    if (!QDir::isAbsolutePath(root) || !QDir().mkpath(root))
        return EnrollmentResult::Unavailable;
    const auto parent = QDir(root).filePath(QStringLiteral("AutomaticPairing"));
    // Older installations may have created an ordinary parent directory. The
    // account approval directory underneath it is the private trust store.
    ::mkdir(QFile::encodeName(parent).constData(), 0700);
    struct stat parentInfo {};
    if (::lstat(QFile::encodeName(parent).constData(), &parentInfo) != 0 ||
        !S_ISDIR(parentInfo.st_mode) || parentInfo.st_uid != ::geteuid())
        return EnrollmentResult::Unavailable;
    const auto path = QDir(parent).filePath(approvalsGroup);
    if (!privateDirectory(path))
        return EnrollmentResult::Unavailable;
    const QDir approvals(path);
    const auto marker = approvals.filePath(id + QStringLiteral(".approved"));
    if (privateEntry(marker, false))
        return EnrollmentResult::Approved;
    if (!confirmed) {
        const auto existing = approvals.entryList({QStringLiteral("*.approved")}, QDir::Files | QDir::NoSymLinks);
        for (const auto &name : existing) {
            if (privateEntry(approvals.filePath(name), false))
                return EnrollmentResult::NeedsConfirmation;
        }
    }
    const int file = ::open(QFile::encodeName(marker).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file >= 0)
        ::close(file);
    return privateEntry(marker, false) ? EnrollmentResult::Approved : EnrollmentResult::Unavailable;
}
#endif
}

QString legacyScope(const ApprovalScope &scope)
{
    if (scope.owner <= 0 || scope.localNode.isEmpty() || scope.adminPort == 0 || scope.certificateSha256.size() != 32)
        return {};
    return QString::number(scope.owner) + QLatin1Char('/') + scope.localNode + QLatin1Char('/') +
        QString::number(scope.adminPort) + QLatin1Char('/') + QString::fromLatin1(scope.certificateSha256.toHex().toUpper());
}

QString approvalId(const ApprovalScope &scope)
{
    const auto legacy = legacyScope(scope);
    if (legacy.isEmpty())
        return {};
    const auto prefix = scope.provider == Provider::Embedded ? QByteArrayLiteral("Embedded/") : QByteArrayLiteral("System/");
    return QString::fromLatin1(QCryptographicHash::hash(prefix + legacy.toUtf8(), QCryptographicHash::Sha256).toHex().toUpper());
}

EnrollmentResult enrollRegistryHost(QSettings &settings, const ApprovalScope &scope, bool confirmed)
{
    const auto id = approvalId(scope);
    if (id.isEmpty())
        return EnrollmentResult::Unavailable;
    const auto previous = settings.value(accountValue);
    if (previous.userType() == QMetaType::QString && previous.toString() == legacyScope(scope))
        return EnrollmentResult::Approved;
    settings.beginGroup(approvalsGroup);
    const bool approved = isApprovedDword(settings.value(id));
    const bool hasApproval = !settings.childKeys().isEmpty() || previous.userType() == QMetaType::QString;
    if (!approved && (!hasApproval || confirmed))
        settings.setValue(id, 1u);
    settings.endGroup();
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return EnrollmentResult::Unavailable;
    return approved || !hasApproval || confirmed ? EnrollmentResult::Approved : EnrollmentResult::NeedsConfirmation;
}

EnrollmentResult enrollLocalHost(const QString &dataDirectory, const ApprovalScope &scope, bool confirmed)
{
#ifdef Q_OS_WIN
    Q_UNUSED(dataDirectory);
    QSettings settings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Aurora\\AutomaticPairing"), QSettings::NativeFormat);
    settings.setFallbacksEnabled(false);
    return enrollRegistryHost(settings, scope, confirmed);
#elif defined(Q_OS_MACOS)
    const auto id = approvalId(scope);
    if (scope.provider == Provider::Embedded && !id.isEmpty())
        return enrollMacHost(dataDirectory, id, confirmed);
#else
    Q_UNUSED(dataDirectory);
    Q_UNUSED(scope);
    Q_UNUSED(confirmed);
#endif
    return EnrollmentResult::Unavailable;
}

}

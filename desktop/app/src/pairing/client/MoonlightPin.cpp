// SPDX-License-Identifier: GPL-3.0-only
#include "MoonlightPin.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSslCertificate>

#include <memory>

namespace Aurora::Pairing::Client::Detail {
namespace {

constexpr int MaxHosts = 4096;
constexpr int MaxCertificateSize = 65536;
constexpr qint64 MaxPortableSize = 16 * 1024 * 1024;

std::unique_ptr<QSettings> openMoonlightSettings(const Stream::Command &command)
{
#ifdef Q_OS_MACOS
    const auto org = QStringLiteral("moonlight-stream.com");
#else
    const auto org = QStringLiteral("Moonlight Game Streaming Project");
#endif
    const auto app = QStringLiteral("Moonlight");
    const QDir directory(command.workingDirectory);
    if (QFileInfo::exists(directory.filePath(QStringLiteral("portable.dat")))) {
        const QFileInfo ini(directory.filePath(org + QLatin1Char('/') + app + QStringLiteral(".ini")));
        if (!ini.isFile() || ini.size() > MaxPortableSize)
            return {};
        return std::make_unique<QSettings>(ini.absoluteFilePath(), QSettings::IniFormat);
    }

    return std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope, org, app);
}

} // namespace

QByteArray readPin(const Stream::Command &command, const QString &hostId)
{
    auto settings = openMoonlightSettings(command);
    if (!settings)
        return {};
    settings->setFallbacksEnabled(false);

    // Moonlight restores a nonempty backup before consulting its main host array.
    int count = settings->beginReadArray(QStringLiteral("hostsbackup"));
    if (!count) {
        settings->endArray();
        count = settings->beginReadArray(QStringLiteral("hosts"));
    }
    if (count <= 0 || count > MaxHosts)
        return {};

    bool found = false;
    QByteArray pem;
    for (int index = 0; index < count; ++index) {
        settings->setArrayIndex(index);
        const auto id = settings->value(QStringLiteral("uuid"));
        if (id.userType() != QMetaType::QString || id.toString() != hostId)
            continue;
        if (found)
            return {};
        found = true;
        const auto value = settings->value(QStringLiteral("srvcert"));
        if (value.userType() != QMetaType::QByteArray)
            return {};
        pem = value.toByteArray();
    }
    settings->endArray();
    if (settings->status() != QSettings::NoError || pem.isEmpty() || pem.size() > MaxCertificateSize)
        return {};

    const auto certificates = QSslCertificate::fromData(pem, QSsl::Pem);
    if (certificates.size() != 1 || certificates.first().isNull())
        return {};
    return certificates.first().digest(QCryptographicHash::Sha256);
}

} // namespace Aurora::Pairing::Client::Detail

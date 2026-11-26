// SPDX-License-Identifier: GPL-3.0-only
#include "SystemStatus.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>

namespace aurora::tailscale::system {
namespace {

QByteArray withoutBom(const QByteArray &bytes)
{
    return bytes.startsWith("\xef\xbb\xbf") ? bytes.mid(3) : bytes;
}

bool asciiLetterOrDigit(QChar ch)
{
    const auto code = ch.unicode();
    return (code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z');
}

bool validProfileId(const QString &id)
{
    if (id.isEmpty() || id.size() > 128 || !asciiLetterOrDigit(id.at(0)))
        return false;
    for (const auto ch : id) {
        if (!asciiLetterOrDigit(ch) && ch != QLatin1Char('-') && ch != QLatin1Char('_') && ch != QLatin1Char('.'))
            return false;
    }
    return true;
}

} // namespace

StatusResult decodeSystemStatus(const QByteArray &bytes)
{
    if (bytes.size() > maxSystemStatusBytes)
        return {StatusError::ResponseTooLarge, {}};
    const auto raw = withoutBom(bytes);
    // Validate the CLI value on its own before embedding it in the shared decoder's envelope.
    // Keep the exact wire bytes so even an owner ID near INT64_MAX survives on Qt 5.
    if (!QJsonDocument::fromJson(raw).isObject())
        return {StatusError::InvalidResponse, {}};
    return decodeStatusReply("{\"id\":\"system-status\",\"ok\":true,\"error\":null,\"result\":" + raw + '}',
        QStringLiteral("system-status"));
}

ProfilesResult decodeProfileList(const QByteArray &bytes)
{
    if (bytes.size() > maxProfilesBytes)
        return {StatusError::ResponseTooLarge, {}};
    const auto document = QJsonDocument::fromJson(withoutBom(bytes));
    if (!document.isArray())
        return {StatusError::InvalidResponse, {}};
    ProfilesResult result{StatusError::None, {}};
    QSet<QString> seen;
    for (const auto &item : document.array()) {
        const auto profile = item.toObject();
        const auto id = profile.value(QLatin1String("id")).toString();
        const auto login = profile.value(QLatin1String("account")).toString();
        if (!validProfileId(id) || login.trimmed().isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        result.profiles.append(Profile{id, profile.value(QLatin1String("nickname")).toString(), login,
            profile.value(QLatin1String("tailnet")).toString(), profile.value(QLatin1String("selected")).toBool()});
    }
    return result;
}

ProfilesResult decodeLegacyProfileList(const QByteArray &bytes)
{
    if (bytes.size() > maxProfilesBytes)
        return {StatusError::ResponseTooLarge, {}};
    auto text = QString::fromUtf8(withoutBom(bytes));
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QTextStream lines(&text, QIODevice::ReadOnly);
    const auto header = lines.readLine();
    const auto tailnetColumn = header.indexOf(QLatin1String("Tailnet"));
    const auto accountColumn = header.indexOf(QLatin1String("Account"));
    if (!header.startsWith(QLatin1String("ID")) || tailnetColumn < 2 || accountColumn <= tailnetColumn)
        return {StatusError::InvalidResponse, {}};

    ProfilesResult result{StatusError::None, {}};
    QSet<QString> seen;
    while (!lines.atEnd()) {
        const auto line = lines.readLine();
        if (line.size() <= accountColumn)
            continue;
        const auto id = line.left(tailnetColumn).trimmed();
        auto login = line.mid(accountColumn).trimmed();
        const bool current = login.endsWith(QLatin1Char('*'));
        if (current) {
            login.chop(1);
            login = login.trimmed();
        }
        if (!validProfileId(id) || login.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        const auto tailnet = line.mid(tailnetColumn, accountColumn - tailnetColumn).trimmed();
        result.profiles.append(Profile{id, {}, login, tailnet, current, false});
    }
    return result;
}

bool cliLacksJsonProfileList(const QByteArray &standardOutput, const QByteArray &standardError)
{
    auto message = QString::fromUtf8(withoutBom(standardError)).trimmed();
    if (message.isEmpty())
        message = QString::fromUtf8(withoutBom(standardOutput)).trimmed();
    const auto detail = message.left(1000);
    return detail.contains(QLatin1String("flag provided but not defined")) && detail.contains(QLatin1String("json"));
}

} // namespace aurora::tailscale::system

// SPDX-License-Identifier: GPL-3.0-only
#include "ServerInfo.h"

#include <QSet>
#include <QXmlStreamReader>

namespace Aurora::Discovery::ServerInfo {

std::optional<StreamHost> parse(const QByteArray &xml, const Endpoint &remote, bool viaEmbeddedTailnet)
{
    if (xml.size() > MaxDocumentSize)
        return std::nullopt;

    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD)
            return std::nullopt;
        if (token == QXmlStreamReader::StartElement)
            break;
    }
    if (!reader.isStartElement() || !reader.namespaceUri().isEmpty() ||
        reader.name() != QLatin1String("root") ||
        reader.attributes().value(QLatin1String("status_code")) != QLatin1String("200"))
        return std::nullopt;

    const static QSet<QString> fields{
        QStringLiteral("PairStatus"), QStringLiteral("appversion"), QStringLiteral("uniqueid"),
        QStringLiteral("hostname"), QStringLiteral("AuroraEncoderStatus"),
        QStringLiteral("AuroraPower"), QStringLiteral("AuroraSessionPrivacy")
    };
    QHash<QString, QString> values;
    while (reader.readNextStartElement()) {
        const auto name = reader.name().toString();
        if (reader.namespaceUri().isEmpty() && fields.contains(name) && !values.contains(name))
            values.insert(name, reader.readElementText(QXmlStreamReader::IncludeChildElements));
        else
            reader.skipCurrentElement();
    }
    while (!reader.atEnd())
        reader.readNext();
    if (reader.hasError() || reader.characterOffset() > MaxDocumentSize ||
        !values.contains(QStringLiteral("PairStatus")) || !values.contains(QStringLiteral("appversion")))
        return std::nullopt;

    const auto id = values.value(QStringLiteral("uniqueid")).trimmed();
    const auto name = values.value(QStringLiteral("hostname")).trimmed();
    if (id.isEmpty() || id.size() > 128 || name.isEmpty())
        return std::nullopt;

    StreamHost host{{remote.address, remote.port}, id, name};
    host.viaEmbeddedTailnet = viaEmbeddedTailnet;
    host.encoderProbePending = values.value(QStringLiteral("AuroraEncoderStatus")) == QLatin1String("probing");
    host.supportsPower = values.value(QStringLiteral("AuroraPower")) == QLatin1String("1");
    host.supportsSessionPrivacy = values.value(QStringLiteral("AuroraSessionPrivacy")) == QLatin1String("1");
    return host;
}

} // namespace Aurora::Discovery::ServerInfo

// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>

class LoopbackFixture final : public QTcpServer
{
public:
    LoopbackFixture(const QString &certPath, const QString &keyPath)
    {
        QFile certFile(certPath);
        QFile keyFile(keyPath);
        if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly))
            return;
        certificate_ = QSslCertificate(certFile.readAll());
        key_ = QSslKey(keyFile.readAll(), QSsl::Rsa);
        if (!certificate_.isNull() && !key_.isNull())
            listen(QHostAddress::LocalHost, 0);
    }

    int connections = 0;
    int receivedRequests = 0;
    qint64 receivedBytes = 0;
    QByteArray authorization;
    QJsonObject config;
    QList<QJsonObject> writes;
    bool ignoreNameOnWrite = false;
    bool failReadback = false;
    bool refuseNextRead = false;
    bool holdNextRead = false;
    bool heldRead = false;

    void releaseRead()
    {
        if (waiting_)
            replyWithConfig(waiting_);
        waiting_.clear();
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        ++connections;
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        socket->setLocalCertificate(certificate_);
        socket->setPrivateKey(key_);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        socket->setProtocol(QSsl::TlsV1_2OrLater);
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QIODevice::readyRead, socket, [this, socket, request = QByteArray{}, handled = false]() mutable {
            const auto bytes = socket->readAll();
            receivedBytes += bytes.size();
            if (handled)
                return;
            request += bytes;
            const auto separator = request.indexOf("\r\n\r\n");
            if (separator < 0)
                return;

            int length = 0;
            for (const auto &line : request.left(separator).split('\n')) {
                const auto boundary = line.indexOf(':');
                if (boundary < 0)
                    continue;
                const auto header = line.left(boundary).toLower();
                const auto value = line.mid(boundary + 1).trimmed();
                if (header == "authorization")
                    authorization = value;
                else if (header == "content-length")
                    length = value.toInt();
            }
            const auto bodyOffset = separator + 4;
            if (request.size() - bodyOffset < length)
                return;

            ++receivedRequests;
            handled = true;
            if (request.startsWith("POST /api/config ")) {
                const auto payload = QJsonDocument::fromJson(request.mid(bodyOffset, length)).object();
                writes.append(payload);
                const auto name = config.value(QStringLiteral("sunshine_name"));
                config = payload;
                if (ignoreNameOnWrite)
                    config.insert(QStringLiteral("sunshine_name"), name);
                if (failReadback)
                    failNextRead_ = true;
                send(socket, "200 OK", R"({"status":true})");
            } else if (holdNextRead) {
                holdNextRead = false;
                heldRead = true;
                waiting_ = socket;
            } else if (refuseNextRead) {
                refuseNextRead = false;
                send(socket, "401 Unauthorized", "{}");
            } else if (failNextRead_) {
                failNextRead_ = false;
                send(socket, "503 Unavailable", "{}");
            } else {
                replyWithConfig(socket);
            }
        });
        socket->startServerEncryption();
    }

private:
    static void send(QSslSocket *socket, const QByteArray &status, const QByteArray &body)
    {
        socket->write("HTTP/1.1 " + status + "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
            QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void replyWithConfig(QSslSocket *socket)
    {
        auto result = config;
        result.insert(QStringLiteral("status"), true);
        send(socket, "200 OK", QJsonDocument(result).toJson(QJsonDocument::Compact));
    }

    QSslCertificate certificate_;
    QSslKey key_;
    QPointer<QSslSocket> waiting_;
    bool failNextRead_ = false;
};

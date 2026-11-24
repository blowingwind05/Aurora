// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>

#include <utility>

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
        connect(socket, &QIODevice::readyRead, socket, [this, socket, request = QByteArray{}]() mutable {
            const auto bytes = socket->readAll();
            receivedBytes += bytes.size();
            request += bytes;
            if (!request.contains("\r\n\r\n"))
                return;

            ++receivedRequests;
            for (const auto &line : request.split('\n')) {
                if (line.startsWith("Authorization:") || line.startsWith("authorization:"))
                    authorization = line.mid(14).trimmed();
            }
            const QByteArray body = R"({"status":true})";
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
        socket->startServerEncryption();
    }

private:
    QSslCertificate certificate_;
    QSslKey key_;
};

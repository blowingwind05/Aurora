// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

class AuroraMore final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString versionLabel READ versionLabel NOTIFY versionLabelChanged)
    Q_PROPERTY(QString feedbackAddress READ feedbackAddress CONSTANT)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

public:
    using OpenUrl = bool (*)(const QUrl &);
    AuroraMore(QNetworkAccessManager &network, OpenUrl openUrl);

    void setApplicationVersion(const QString &version);
    QString versionLabel() const;
    QString feedbackAddress() const;
    bool checking() const { return updateInProgress; }

    Q_INVOKABLE void request(const QString &action);

signals:
    void versionLabelChanged();
    void checkingChanged();
    void notice(const QString &message);

private:
    void checkUpdates();
    bool readResponse();
    void replyFinished();
    void finish(const QString &message);

    QNetworkAccessManager &network;
    const OpenUrl openUrl;
    QNetworkReply *activeReply = nullptr;
    QTimer deadline;
    QByteArray response;
    QString currentVersion;
    bool hasSnapshot = false;
    bool updateInProgress = false;
};

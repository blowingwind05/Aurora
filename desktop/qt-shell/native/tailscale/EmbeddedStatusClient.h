// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Status.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

namespace aurora::tailscale {

// Own one helper for one state directory, on the coordinator's event-loop thread.
class EmbeddedStatusClient final : public QObject
{
    Q_OBJECT

public:
    EmbeddedStatusClient(QString executable, QString stateDirectory, QObject *parent = nullptr);
    ~EmbeddedStatusClient() override;

    // Windows package root or the macOS bundle's Contents/MacOS directory.
    static QString bundledExecutable(const QString &applicationDirectory);
    bool isAvailable() const;

    // False while another request or a failed helper's shutdown is in progress.
    // An accepted request completes asynchronously with the supplied context generation.
    bool requestStatus(quint64 generation);

signals:
    void statusFinished(quint64 generation, const aurora::tailscale::StatusResult &result);

private:
    void begin();
    void sendRequest();
    void receive();
    void finish(StatusResult result, bool stopHelper = false);
    void stop();

    struct Request
    {
        quint64 generation;
        QString id;
        bool sent = false;
    };

    const QString executable;
    const QString stateDirectory;
    QProcess process;
    QTimer deadline;
    QElapsedTimer elapsed;
    QByteArray output;
    std::optional<Request> request;
    quint64 serial = 0;
    bool stopping = false;
};

} // namespace aurora::tailscale

// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace Aurora::Sunshine {

struct PendingPairing
{
    QString id;
    QString name;
    QString address;
    std::optional<QHostAddress> sourceIpv4;
    std::optional<quint16> sourcePort;
};

struct AdminIdentity
{
    quint16 port = 0;
    QByteArray certificateSha256;
};

// The supervisor calls authorized only after it has validated and approved this exact request.
class SupervisorPairingApproval
{
public:
    static SupervisorPairingApproval authorized(QString pairingId, QString pin, QString name);
    static SupervisorPairingApproval clientGrant(QString name, QByteArray challenge,
        QByteArray certificate, QByteArray signature);
    static SupervisorPairingApproval expireClientGrants();

private:
    friend class BundledAdmin;
    SupervisorPairingApproval(QString pairingId, QString pin, QString name);
    struct ClientGrant {
        QByteArray challenge;
        QByteArray certificate;
        QByteArray signature;
    };

    QString id_;
    QString pin_;
    QString name_;
    std::optional<ClientGrant> grant_;
    bool expireGrants_ = false;
};

class BundledAdmin final : public QObject
{
public:
    enum class Error {
        None,
        Tls,
        Timeout,
        Network,
        Unauthorized,
        HttpStatus,
        ResponseTooLarge,
        InvalidResponse,
        Rejected,
        InvalidPayload,
        ReadbackMismatch,
        Cancelled,
        StaleSession,
        Unavailable,
    };

    struct Result
    {
        Error error = Error::None;
        int httpStatus = 0;
        QString message;
        QJsonObject config;

        explicit operator bool() const { return error == Error::None; }
    };

    struct PendingResult
    {
        Error error = Error::None;
        int httpStatus = 0;
        QString message;
        QList<PendingPairing> pairings;
        bool supportsClientGrant = false;

        explicit operator bool() const { return error == Error::None; }
    };

    struct ApprovalResult
    {
        Error error = Error::None;
        int httpStatus = 0;
        QString message;

        explicit operator bool() const { return error == Error::None; }
    };

    using PendingCompletion = std::function<void(PendingResult)>;
    using ApprovalCompletion = std::function<void(ApprovalResult)>;
    using ApprovalStillCurrent = std::function<bool()>;
    using Completion = std::function<void(Result)>;
    using PreparePayload = std::function<std::optional<QJsonObject>(const QJsonObject &current, QString &error)>;
    using VerifyReadback = std::function<QString(const QJsonObject &actual)>;

    // profileDirectory is the bundled host's config directory, passed by its lifecycle owner.
    // launchPassword is the same 64 ASCII hex bytes sent to the current host over stdin.
    static std::unique_ptr<BundledAdmin> open(const QString &profileDirectory,
        const QByteArray &launchPassword, QString &error);
    ~BundledAdmin() override;
    AdminIdentity identity() const { return {port_, pin_}; }

    // Use the object's thread with a running event loop. A false return means the operation
    // could not be started and no callback will run. Destroy this object to cancel pending work.
    bool readConfig(Completion completion);

    // Fetch fresh config, build and submit a complete payload, then fetch and verify readback.
    // prepare returns nullopt on failure. verify returns an empty string on success.
    bool saveConfig(PreparePayload prepare, VerifyReadback verify, Completion completion);

    // Read-only pairing lookup and a separately authorized, exact-ID approval.
    // The optional guard runs on this thread at admission and before HTTP transmission.
    bool readPendingPairings(PendingCompletion completion);
    bool approvePairing(SupervisorPairingApproval approval, ApprovalCompletion completion,
                        ApprovalStillCurrent stillCurrent = {});

private:
    enum class Endpoint { Config, PendingPairings, PairApproval, ClientGrant, ExpireClientGrants };
    class Exchange;
    friend struct BundledAdminFixture;

    static std::unique_ptr<BundledAdmin> openAtPort(const QString &profileDirectory,
        const QByteArray &launchPassword, quint16 port, QString &error);
    BundledAdmin(quint16 port, QByteArray pin, QByteArray authorization);

    void send(Endpoint endpoint, std::optional<QByteArray> body, Completion completion,
              ApprovalStillCurrent stillCurrent = {});
    void finishOperation(Completion &completion, Result result);

    const quint16 port_;
    const QByteArray pin_;
    QByteArray authorization_;
    bool busy_ = false;
};

} // namespace Aurora::Sunshine

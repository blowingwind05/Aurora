// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace Aurora::Sunshine {

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
    };

    struct Result
    {
        Error error = Error::None;
        int httpStatus = 0;
        QString message;
        QJsonObject config;

        explicit operator bool() const { return error == Error::None; }
    };

    using Completion = std::function<void(Result)>;
    using PreparePayload = std::function<std::optional<QJsonObject>(const QJsonObject &current, QString &error)>;
    using VerifyReadback = std::function<QString(const QJsonObject &actual)>;

    // profileDirectory is the bundled host's config directory, passed by its lifecycle owner.
    // launchPassword is the same 64 ASCII hex bytes sent to the current host over stdin.
    static std::unique_ptr<BundledAdmin> open(const QString &profileDirectory,
        const QByteArray &launchPassword, QString &error);
    ~BundledAdmin() override;

    // Use the object's thread with a running event loop. A false return means the operation
    // could not be started and no callback will run. Destroy this object to cancel pending work.
    bool readConfig(Completion completion);

    // Fetch fresh config, build and submit a complete payload, then fetch and verify readback.
    // prepare returns nullopt on failure. verify returns an empty string on success.
    bool saveConfig(PreparePayload prepare, VerifyReadback verify, Completion completion);

private:
    friend struct BundledAdminFixture;

    static std::unique_ptr<BundledAdmin> openAtPort(const QString &profileDirectory,
        const QByteArray &launchPassword, quint16 port, QString &error);
    BundledAdmin(quint16 port, QByteArray pin, QByteArray authorization);

    void sendConfig(std::optional<QByteArray> body, Completion completion);
    void finishOperation(Completion &completion, Result result);

    const quint16 port_;
    const QByteArray pin_;
    QByteArray authorization_;
    bool busy_ = false;
};

} // namespace Aurora::Sunshine

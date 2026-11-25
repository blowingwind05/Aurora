// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "PairingClient.h"
#include "../../adaptors/tailscale/Status.h"

#include <QTimer>

#include <functional>
#include <memory>

namespace aurora::tailscale {
class EmbeddedStatusClient;
struct PairingLeaseResult;
namespace system { class SystemPairingNc; }
}

namespace Aurora::Pairing::Client {

// Owns the authenticated channel and its lease from opening through completion.
class TailnetExchange final : public QObject
{
    Q_OBJECT

public:
    explicit TailnetExchange(aurora::tailscale::EmbeddedStatusClient *tailnet, QObject *parent = nullptr);
    ~TailnetExchange() override;

    using StillCurrent = std::function<bool()>;
    quint64 start(Request request, Discovery::NetworkSnapshot network,
        aurora::tailscale::AccountIdentity account, bool embedded, StillCurrent stillCurrent);
    void cancel(quint64 exchange);

signals:
    void finished(quint64 exchange, const Aurora::Pairing::Client::Result &result);

private:
    struct Attempt;
    void open();
    void requestEmbeddedChannel();
    void embeddedChannelOpened(quint64 generation, quint64 request, const aurora::tailscale::PairingLeaseResult &result);
    void startLoopback();
#ifdef Q_OS_WIN
    void startStdio(quint64 request, QIODevice *channel);
#endif
    bool current() const;
    void finish(Result result);
    void release();

    QPointer<aurora::tailscale::EmbeddedStatusClient> tailnet_;
    Coordinator coordinator_{this};
#ifdef Q_OS_WIN
    std::unique_ptr<aurora::tailscale::system::SystemPairingNc> system_;
#endif
    QTimer retry_{this};
    std::unique_ptr<Attempt> attempt_;
    quint64 nextExchange_ = 0;
};

} // namespace Aurora::Pairing::Client

// SPDX-License-Identifier: GPL-3.0-only
#include "../../desktop/protocol/bitrate/ClientProtocol.h"
#include "../../desktop/protocol/bitrate/HostControl.h"

#include <QtGlobal>

#include <future>
#include <thread>

namespace {

void require(bool condition, const char* message)
{
    if (!condition)
        qFatal("%s", message);
}

} // namespace

int main()
{
    namespace HostBitrate = aurora::video_bitrate;
    namespace ClientBitrate = AuroraBitrate;

    const std::string token = HostBitrate::make_token({0x18, 0xfe}, 1);
    const ClientBitrate::Request statusRequest{};
    const auto statusQuery = ClientBitrate::query(statusRequest);
    const auto status = HostBitrate::parse(statusQuery.toStdString(), true);
    require(status && status->token.empty(), "Host accepts the client's status request");

    const HostBitrate::budget_t budget{12000, 20, 192, 0};
    HostBitrate::control_t control(token, budget);
    require(control.status() == "unavailable", "Control requires an attached encoder");
    control.attach(budget.encoder_kbps(budget.negotiated_kbps));
    require(!ClientBitrate::validatedLine(statusRequest, QByteArray::fromStdString(control.status())).isEmpty(),
            "Client accepts the host's status response");

    const ClientBitrate::Request set{QByteArray::fromStdString(token), "6000", 0};
    const auto query = ClientBitrate::query(set).toStdString();
    const auto change = HostBitrate::parse(query, true);
    require(change && change->kbps == 6000, "Host parses the client's requested bitrate");
    require(!HostBitrate::parse(query, false), "Host requires an empty request body");
    require(control.submit({token, budget.negotiated_kbps + 1, 0}) == "rejected",
            "An out-of-budget request is rejected before consuming a revision");
    require(control.submit(*change).empty(), "Host queues a change in the negotiated budget");
    require(control.submit(*change) == "busy", "Host only queues one change at a time");

    control.apply([](int target) { return target; });
    const auto applied = QByteArray::fromStdString(control.await());
    require(!ClientBitrate::validatedLine(set, applied).isEmpty(), "Client accepts the matching encoder acknowledgement");
    const ClientBitrate::Request anotherTarget{set.session, "8000"};
    require(ClientBitrate::validatedLine(anotherTarget, applied).isEmpty(), "Client rejects an acknowledgement for another target");

    ClientBitrate::Confirmation confirmed;
    require(ClientBitrate::parseConfirmation(set, applied, confirmed) && confirmed.revision == 1, "An accepted change consumes exactly one revision");
    require(control.submit(*change) == "stale", "Repeating an old revision cannot overwrite the result");
    const ClientBitrate::Request wrongRevision{set.session, set.kbps, 1};
    require(ClientBitrate::validatedLine(wrongRevision, applied).isEmpty(), "Client rejects an acknowledgement for the wrong revision");

    require(control.submit({token, 8000, 1}).empty(), "A fresh revision may be queued");
    require(control.await() == "expired", "A request that never reached the driver expires");
    control.apply([](int) {
        qFatal("An expired request must never enter the driver");
        return 0;
    });
    require(control.submit({token, 8000, 1}) == "stale", "Expiry still consumes the queued revision");

    require(control.submit({token, 8000, 2}).empty(), "The next revision can be accepted");
    std::promise<void> applying;
    std::promise<void> finish;
    auto release = finish.get_future();
    std::thread driver([&] {
        control.apply([&](int target) {
            applying.set_value();
            release.wait();
            return target;
        });
    });
    applying.get_future().wait();
    require(control.await() == "pending", "A started driver call has an unresolved result after timeout");
    require(control.status() == "busy", "Status cannot advertise a writable revision while the driver is still running");
    require(control.submit({token, 9000, 3}) == "busy", "Timeout does not free an executing slot");
    finish.set_value();
    driver.join();
    ClientBitrate::Confirmation recovered;
    require(ClientBitrate::parseConfirmation(statusRequest, QByteArray::fromStdString(control.status()), recovered) &&
            recovered.requestedKbps == 8000 && recovered.revision == 3,
            "Status confirms the eventual result of a timed-out driver call");

    require(control.submit({token, 9000, 3}).empty(), "Another change can be accepted after reconciliation");
    control.detach();
    require(control.await() == "unavailable", "Encoder teardown cancels queued work");
    require(control.resume_target() == recovered.encoderKbps, "A rebuild resumes at the last confirmed target");
    control.attach(control.resume_target());
    require(control.submit({token, 9000, 3}) == "stale", "Rebuild does not reuse a consumed revision");
    require(control.submit({token, 9000, 4}).empty(), "A rebuilt encoder can accept the next revision");
    control.apply([](int) { return 0; });
    require(control.await() == "failed", "Driver rejection is a definitive failure");
    require(ClientBitrate::parseConfirmation(statusRequest, QByteArray::fromStdString(control.status()), recovered) &&
            recovered.requestedKbps == 8000 && recovered.revision == 5,
            "Driver rejection preserves the last confirmed rate and consumes the revision");
    control.close();
    require(control.status() == "unavailable", "Ending the session revokes status");
    require(control.submit(*change) == "unavailable", "Ending the session revokes changes");
    HostBitrate::control_t unsupported(token, budget);
    unsupported.attach(0);
    require(unsupported.status() == "unsupported", "Unsupported encoders can be distinguished from rebuilding encoders");
}

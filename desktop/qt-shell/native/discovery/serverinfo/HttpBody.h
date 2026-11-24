// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "ServerInfo.h"

namespace Aurora::Discovery::ServerInfo::Detail {

// Bounded framing for the single, non-pipelined Sunshine response.
class HttpBody
{
public:
    enum class Result { Pending, Complete, Invalid };

    Result append(const QByteArray &bytes);
    Result end();
    const QByteArray &body() const { return body_; }

private:
    enum class State { Headers, Fixed, CloseDelimited, ChunkSize, ChunkData, ChunkEnd, Trailers };

    bool readHeaders(const QByteArray &headers);
    bool copyBody(const char *data, int count);
    void consume();

    QByteArray buffer_;
    QByteArray body_;
    int wireBytes_ = 0;
    int remaining_ = 0;
    State state_ = State::Headers;
    Result result_ = Result::Pending;
};

} // namespace Aurora::Discovery::ServerInfo::Detail

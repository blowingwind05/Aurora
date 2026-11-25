// SPDX-License-Identifier: GPL-3.0-only
#include "HttpBody.h"

#include <QList>

#include <algorithm>
#include <optional>

namespace Aurora::Discovery::ServerInfo::Detail {
namespace {

constexpr int MaxHeaders = 16384;
constexpr int MaxChunkLine = 1024;
constexpr int MaxWireSize = MaxHeaders + MaxDocumentSize * 4;

bool decimal(char ch)
{
    return ch >= '0' && ch <= '9';
}

bool hexadecimal(char ch)
{
    return decimal(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

} // namespace

HttpBody::Result HttpBody::append(const QByteArray &bytes)
{
    if (result_ != Result::Pending)
        return result_;
    if (bytes.size() > MaxWireSize - wireBytes_) {
        result_ = Result::Invalid;
        return result_;
    }
    wireBytes_ += static_cast<int>(bytes.size());
    buffer_ += bytes;
    consume();
    return result_;
}

HttpBody::Result HttpBody::end()
{
    if (result_ == Result::Pending)
        result_ = state_ == State::CloseDelimited ? Result::Complete : Result::Invalid;
    return result_;
}

bool HttpBody::readHeaders(const QByteArray &headers)
{
    const auto lines = headers.split('\n');
    const auto status = lines.first().trimmed();
    if ((!status.startsWith("HTTP/1.1 ") && !status.startsWith("HTTP/1.0 ")) || status.size() < 12 ||
        !decimal(status.at(9)) || !decimal(status.at(10)) || !decimal(status.at(11)) ||
        (status.size() > 12 && status.at(12) != ' '))
        return false;
    const auto code = status.mid(9, 3).toInt();
    if (code >= 100 && code < 200)
        return code != 101; // Keep reading the final response after informational headers.
    if (code < 200 || code >= 300)
        return false;

    std::optional<int> length;
    bool chunked = false;
    for (auto line = lines.cbegin() + 1; line != lines.cend(); ++line) {
        const auto separator = line->indexOf(':');
        if (separator <= 0 || line->startsWith(' ') || line->startsWith('\t'))
            return false;
        const auto name = line->left(separator).toLower();
        const auto value = line->mid(separator + 1).trimmed();
        if (name == "content-length") {
            if (value.isEmpty() || !std::all_of(value.cbegin(), value.cend(), decimal))
                return false;
            bool ok = false;
            const int size = value.toInt(&ok);
            if (!ok || size > MaxDocumentSize || (length && size != *length))
                return false;
            length = size;
        } else if (name == "transfer-encoding") {
            if (chunked || value.toLower() != "chunked")
                return false;
            chunked = true;
        } else if (name == "content-encoding" && !value.isEmpty() && value.toLower() != "identity") {
            return false;
        }
    }
    if (chunked && length)
        return false;
    state_ = chunked ? State::ChunkSize : length ? State::Fixed : State::CloseDelimited;
    remaining_ = length.value_or(0);
    return true;
}

bool HttpBody::copyBody(const char *data, int count)
{
    if (count > MaxDocumentSize - body_.size())
        return false;
    body_.append(data, count);
    return true;
}

void HttpBody::consume()
{
    decltype(buffer_.size()) pos = 0;
    while (result_ == Result::Pending) {
        const auto remainingBytes = buffer_.size() - pos;
        if (state_ == State::Headers) {
            const auto end = buffer_.indexOf("\r\n\r\n", pos);
            if (end < 0) {
                if (remainingBytes >= MaxHeaders)
                    result_ = Result::Invalid;
                break;
            }
            if (end - pos + 4 > MaxHeaders || !readHeaders(buffer_.mid(pos, end - pos))) {
                result_ = Result::Invalid;
                break;
            }
            pos = end + 4;
        } else if (state_ == State::Fixed || state_ == State::ChunkData) {
            const int take = std::min(static_cast<int>(remainingBytes), remaining_);
            if (!copyBody(buffer_.constData() + pos, take)) {
                result_ = Result::Invalid;
                break;
            }
            pos += take;
            remaining_ -= take;
            if (remaining_ > 0)
                break;
            if (state_ == State::Fixed)
                result_ = Result::Complete;
            else
                state_ = State::ChunkEnd;
        } else if (state_ == State::CloseDelimited) {
            if (!copyBody(buffer_.constData() + pos, static_cast<int>(remainingBytes)))
                result_ = Result::Invalid;
            pos = buffer_.size();
            break;
        } else if (state_ == State::ChunkEnd) {
            if (remainingBytes < 2)
                break;
            if (buffer_.at(pos) != '\r' || buffer_.at(pos + 1) != '\n') {
                result_ = Result::Invalid;
                break;
            }
            pos += 2;
            state_ = State::ChunkSize;
        } else {
            const auto end = buffer_.indexOf("\r\n", pos);
            const auto limit = state_ == State::ChunkSize ? MaxChunkLine : MaxHeaders;
            if (end < 0) {
                if (remainingBytes >= limit)
                    result_ = Result::Invalid;
                break;
            }
            if (end - pos + 2 > limit) {
                result_ = Result::Invalid;
                break;
            }
            const auto line = buffer_.mid(pos, end - pos);
            pos = end + 2;
            if (state_ == State::Trailers) {
                if (line.isEmpty())
                    result_ = Result::Complete;
                continue;
            }

            const auto extension = line.indexOf(';');
            const auto sizeText = extension < 0 ? line : line.left(extension);
            bool ok = false;
            const int size = sizeText.toInt(&ok, 16);
            if (!ok || !std::all_of(sizeText.cbegin(), sizeText.cend(), hexadecimal) ||
                size > MaxDocumentSize - body_.size()) {
                result_ = Result::Invalid;
                break;
            }
            remaining_ = size;
            state_ = size ? State::ChunkData : State::Trailers;
        }
    }
    buffer_.remove(0, pos);
}

} // namespace Aurora::Discovery::ServerInfo::Detail

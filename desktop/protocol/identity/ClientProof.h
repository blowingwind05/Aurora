// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace Aurora {
namespace ClientProof {

constexpr std::size_t DigestSize = 32;
constexpr std::size_t ChallengeSize = 3 * DigestSize;
constexpr std::size_t MaxCertificateBytes = 4096;
constexpr std::size_t MaxSignatureBytes = 1024;
constexpr char Domain[] = "aurora-client-proof/1";

using Challenge = std::array<unsigned char, ChallengeSize>;
using CertificateDigest = std::array<unsigned char, DigestSize>;
using Transcript = std::array<unsigned char, sizeof(Domain) + ChallengeSize + DigestSize>;

// Challenge bytes are the random nonce, host certificate SHA-256, and a
// digest of the Tailscale authorization scope, in that order.
inline Transcript transcript(const Challenge &challenge, const CertificateDigest &certificateDigest)
{
    Transcript result{};
    auto next = std::copy_n(Domain, sizeof(Domain), result.begin());
    next = std::copy(challenge.begin(), challenge.end(), next);
    std::copy(certificateDigest.begin(), certificateDigest.end(), next);
    return result;
}

} // namespace ClientProof
} // namespace Aurora

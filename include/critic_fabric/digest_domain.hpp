// Critic Fabric — domain separation tags for canonical digests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_DIGEST_DOMAIN_HPP
#define CRITIC_FABRIC_DIGEST_DOMAIN_HPP

#include <cstdint>

namespace critic_fabric {

// Distinct domain tags prevent a digest computed over one kind of object from
// ever colliding with a digest over another kind, even when the byte encodings
// happen to line up.
enum class ContentDigestDomain : std::uint32_t {
    Target = 0x54415247u,
    Evidence = 0x45564944u,
    Finding = 0x46494E44u,
    Policy = 0x504F4C43u,
    Explanation = 0x4558504Cu,
    Decision = 0x44454349u,
    State = 0x53544154u,
    JournalRecord = 0x4A524E4Cu,
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_DIGEST_DOMAIN_HPP

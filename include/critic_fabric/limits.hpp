// Critic Fabric — explicit resource bounds.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_LIMITS_HPP
#define CRITIC_FABRIC_LIMITS_HPP

#include <cstdint>

namespace critic_fabric {

// Hard compile-time ceilings. A runtime configuration may lower these but can
// never raise them, so no configuration file can turn an untrusted length field
// into an unbounded allocation.
inline constexpr std::uint32_t kHardMaxFrameBytes = 4u * 1024u * 1024u;
inline constexpr std::uint32_t kHardMaxPayloadBytes = 1u * 1024u * 1024u;
inline constexpr std::uint32_t kHardMaxMetadataBytes = 64u * 1024u;
inline constexpr std::uint32_t kHardMaxFindingsPerReview = 4096u;
inline constexpr std::uint32_t kHardMaxCriticsPerReview = 256u;
inline constexpr std::uint32_t kHardMaxReviewRounds = 64u;
inline constexpr std::uint32_t kHardMaxRetries = 16u;

struct RuntimeLimits {
    std::uint32_t max_critics_per_review = 32;
    std::uint32_t max_concurrent_reviews = 512;
    std::uint32_t max_retained_reviews = 8192;
    std::uint32_t max_retained_targets = 262144;
    std::uint32_t max_in_flight_findings = 1024;
    std::uint32_t max_findings_per_review = 256;
    std::uint32_t max_evidence_per_finding = 16;
    std::uint32_t max_evidence_records_per_review = 4096;
    std::uint32_t max_evidence_payload_bytes = 8192;
    std::uint32_t max_explanation_bytes = 2048;
    std::uint32_t max_challenges_per_review = 64;
    std::uint32_t max_rebuttals_per_challenge = 4;
    std::uint32_t max_review_rounds = 8;
    std::uint32_t max_critic_retries = 4;
    std::uint32_t max_worker_connections = 128;
    std::uint32_t max_retained_generations = 64;
    std::uint32_t max_target_payload_bytes = 65536;
    std::uint32_t max_metadata_string_bytes = 1024;
    std::uint32_t max_frame_bytes = 1u * 1024u * 1024u;
    std::uint32_t max_persisted_records = 1u * 1024u * 1024u;
    std::uint32_t max_persistence_file_bytes = 64u * 1024u * 1024u;

    // Clamps every field into the hard ceilings. Called on any externally
    // supplied configuration before the limits are used for allocation.
    void clamp_to_hard_ceiling() noexcept;
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_LIMITS_HPP

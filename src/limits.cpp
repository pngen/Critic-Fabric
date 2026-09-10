// Critic Fabric — resource bound clamping.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/limits.hpp"

namespace critic_fabric {
namespace {

template <typename T>
void clamp_into(T& value, T low, T high) noexcept {
    if (value < low) {
        value = low;
    }
    if (value > high) {
        value = high;
    }
}

}  // namespace

void RuntimeLimits::clamp_to_hard_ceiling() noexcept {
    clamp_into(max_critics_per_review, 1u, kHardMaxCriticsPerReview);
    clamp_into(max_concurrent_reviews, 1u, 65536u);
    clamp_into(max_retained_reviews, 1u, 1048576u);
    clamp_into(max_retained_targets, 1u, 16777216u);
    clamp_into(max_in_flight_findings, 1u, kHardMaxFindingsPerReview);
    clamp_into(max_findings_per_review, 1u, kHardMaxFindingsPerReview);
    clamp_into(max_evidence_per_finding, 1u, 4096u);
    clamp_into(max_evidence_records_per_review, 1u, 65536u);
    clamp_into(max_evidence_payload_bytes, 1u, kHardMaxPayloadBytes);
    clamp_into(max_explanation_bytes, 1u, kHardMaxPayloadBytes);
    clamp_into(max_challenges_per_review, 1u, 65536u);
    clamp_into(max_rebuttals_per_challenge, 1u, 1024u);
    clamp_into(max_review_rounds, 1u, kHardMaxReviewRounds);
    clamp_into(max_critic_retries, 1u, kHardMaxRetries);
    clamp_into(max_worker_connections, 1u, 4096u);
    clamp_into(max_retained_generations, 1u, 4096u);
    clamp_into(max_target_payload_bytes, 1u, kHardMaxPayloadBytes);
    clamp_into(max_metadata_string_bytes, 1u, kHardMaxMetadataBytes);
    clamp_into(max_frame_bytes, 64u, kHardMaxFrameBytes);
    clamp_into(max_persisted_records, 1u, 16777216u);
    clamp_into(max_persistence_file_bytes, 1024u, 1073741824u);
}

}  // namespace critic_fabric

// Critic Fabric — structured, attributable evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_EVIDENCE_HPP
#define CRITIC_FABRIC_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "critic_fabric/digest.hpp"
#include "critic_fabric/digest_domain.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"

namespace critic_fabric {

// Evidence is bound to an exact target generation and carries explicit
// provenance and integrity state. UNKNOWN provenance and unverified integrity
// can never satisfy a mandatory verification predicate.
struct EvidenceRecord {
    EvidenceId id{};
    EvidenceGeneration generation = EvidenceGeneration::first();
    EvidenceSourceType source_type = EvidenceSourceType::Other;
    EvidenceProvenance provenance = EvidenceProvenance::Unknown;
    EvidenceIntegrity integrity = EvidenceIntegrity::Unverified;
    ProducerId producer{};
    TargetId target{};
    TargetGeneration target_generation{};
    Digest target_digest{};
    ReviewId review{};
    ReviewGeneration review_generation{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    FindingId finding{};
    Digest content_digest{};
    std::vector<std::uint8_t> payload;
    std::string reference;
    std::uint64_t created_order = 0;
    bool current = true;
    bool superseded = false;
    EvidenceId superseded_by{};

    [[nodiscard]] Digest compute_content_digest() const;
};

struct SubmitEvidenceRequest {
    EvidenceSourceType source_type = EvidenceSourceType::Other;
    EvidenceProvenance provenance = EvidenceProvenance::Unknown;
    EvidenceIntegrity integrity = EvidenceIntegrity::Unverified;
    ProducerId producer{};
    std::string reference;
    std::vector<std::uint8_t> payload;
    FindingId finding{};
    CoordinatorEpoch epoch{};
    RequestId request{};
    // Authority binding supplied by the caller; validated against live state.
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_EVIDENCE_HPP

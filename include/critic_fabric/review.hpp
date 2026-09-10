// Critic Fabric — assignments, review state, decisions, and snapshots.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_REVIEW_HPP
#define CRITIC_FABRIC_REVIEW_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "critic_fabric/challenge.hpp"
#include "critic_fabric/digest.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/evidence.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/ids.hpp"
#include "critic_fabric/policy.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

// Grants one logical critic, at one exact generation, on one physical worker
// incarnation, the right to publish current findings for one review.
struct CriticAssignment {
    CriticAssignmentId id{};
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    CriticRole role = CriticRole::GeneralCritic;
    bool required = false;
    std::uint32_t weight = 1;
    std::uint64_t assigned_order = 0;
    bool active = true;
    AttemptGeneration attempt = AttemptGeneration::first();
    std::string retirement_reason;
};

struct ReviewRecord {
    ReviewId id{};
    ReviewRequestId request{};
    ReviewGeneration generation = ReviewGeneration::first();
    TargetId target{};
    TargetGeneration target_generation{};
    Digest target_digest{};
    ReviewSpec spec;
    ReviewState state = ReviewState::Declared;
    ReviewDisposition disposition = ReviewDisposition::None;
    std::uint64_t declared_order = 0;
    std::uint64_t updated_order = 0;
    ReviewGeneration superseded_by{};
    std::string cancellation_reason;
    std::string status_detail;
    std::uint32_t rounds_used = 0;
    bool authority_current = true;
};

// The single authoritative logical review result for one review generation.
struct Decision {
    DecisionId id{};
    DecisionGeneration generation = DecisionGeneration::first();
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    ReviewDisposition disposition = ReviewDisposition::None;
    Digest decision_digest{};
    Digest policy_digest{};
    Digest evidence_basis_digest{};
    std::uint64_t committed_order = 0;
    CoordinatorEpoch epoch{};
};

struct ReviewSummary {
    ReviewId id{};
    ReviewGeneration generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    ReviewState state = ReviewState::Declared;
    ReviewDisposition disposition = ReviewDisposition::None;
    std::uint32_t finding_count = 0;
    std::uint32_t current_finding_count = 0;
    std::uint32_t assignment_count = 0;
    bool has_decision = false;
};

// A fully detached value copy of review state. Query APIs never return
// references into mutable coordinator internals.
struct ReviewSnapshot {
    ReviewRecord review;
    std::vector<CriticAssignment> assignments;
    std::vector<Finding> findings;
    std::vector<EvidenceRecord> evidences;
    std::vector<Challenge> challenges;
    std::vector<Rebuttal> rebuttals;
    std::optional<Decision> decision;
    Digest state_digest{};
};

struct DeclareReviewRequest {
    ReviewRequestId request{};
    TargetId target{};
    TargetGeneration target_generation{};
    ReviewSpec spec;
    CoordinatorEpoch epoch{};
};

struct AssignCriticsRequest {
    ReviewId review{};
    ReviewGeneration review_generation{};
    struct Assignment {
        CriticId critic{};
        CriticGeneration critic_generation{};
        WorkerId worker{};
        WorkerBootId worker_boot{};
        CriticRole role = CriticRole::GeneralCritic;
        bool required = false;
    };
    std::vector<Assignment> assignments;
    CoordinatorEpoch epoch{};
    RequestId request{};
};

struct CancelReviewRequest {
    ReviewId review{};
    ReviewGeneration review_generation{};
    std::string reason;
    CoordinatorEpoch epoch{};
    RequestId request{};
};

struct SupersedeTargetRequest {
    TargetId target{};
    TargetClass target_class = TargetClass::Claim;
    std::string schema_name;
    std::vector<std::uint8_t> payload;
    ProducerId producer{};
    std::string producer_version;
    std::string provenance;
    std::string reason;
    CoordinatorEpoch epoch{};
    RequestId request{};
};

struct CommitReviewRequest {
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetGeneration target_generation{};
    CoordinatorEpoch epoch{};
    RequestId request{};
    // Deterministic explanation is always produced; the flag exists so callers
    // can request the rendered text explicitly.
    bool include_rendered_explanation = false;
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_REVIEW_HPP

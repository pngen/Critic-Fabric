// Critic Fabric — first-class findings with explicit generation binding.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_FINDING_HPP
#define CRITIC_FABRIC_FINDING_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "critic_fabric/digest.hpp"
#include "critic_fabric/digest_domain.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"

namespace critic_fabric {

// Confidence is advisory metadata only. It is deliberately excluded from hard
// predicates, quorum computation, weighting, and the authoritative disposition,
// because a self-declared score has no rigorous semantics.
struct Confidence {
    bool provided = false;
    std::uint32_t basis_points = 0;  // 0..10000, advisory
    ConfidenceBasis basis = ConfidenceBasis::NotProvided;

    // Compile-time and run-time statement of intent: confidence never decides.
    [[nodiscard]] static constexpr bool is_authoritative() noexcept { return false; }
};

struct Finding {
    FindingId id{};
    FindingGeneration generation = FindingGeneration::first();
    FindingId supersedes{};
    FindingId superseded_by{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    CriticAssignmentId assignment{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    AttemptGeneration attempt = AttemptGeneration::first();
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    FindingCategory category = FindingCategory::Other;
    Severity severity = Severity::Info;
    FindingOutcome outcome = FindingOutcome::Unknown;
    std::vector<EvidenceId> evidence;
    std::string explanation;
    Confidence confidence;
    std::string provenance;
    std::uint64_t creation_order = 0;
    FindingState state = FindingState::Current;
    ChallengeId challenged_by{};
    Digest finding_digest{};

    [[nodiscard]] Digest compute_finding_digest() const;
    [[nodiscard]] bool is_current() const noexcept { return state == FindingState::Current; }
};

struct SubmitFindingRequest {
    FindingCategory category = FindingCategory::Other;
    Severity severity = Severity::Info;
    FindingOutcome outcome = FindingOutcome::Unknown;
    std::vector<EvidenceId> evidence;
    std::string explanation;
    Confidence confidence;
    std::string provenance;
    FindingId supersedes{};
    CoordinatorEpoch epoch{};
    RequestId request{};
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

#endif  // CRITIC_FABRIC_FINDING_HPP

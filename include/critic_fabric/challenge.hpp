// Critic Fabric — bounded challenge and rebuttal.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_CHALLENGE_HPP
#define CRITIC_FABRIC_CHALLENGE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"

namespace critic_fabric {

// A challenge disputes a specific predicate or evidence item of one finding.
// Rounds are bounded by review policy; there is no open-ended debate.
struct Challenge {
    ChallengeId id{};
    ChallengeGeneration generation = ChallengeGeneration::first();
    FindingId challenged_finding{};
    FindingGeneration challenged_finding_generation{};
    CriticId challenger{};
    CriticGeneration challenger_generation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    std::string disputed_predicate;
    std::string rationale;
    std::vector<EvidenceId> evidence;
    std::uint64_t creation_order = 0;
    ChallengeOutcome outcome = ChallengeOutcome::Unresolved;
    bool resolved = false;
    RebuttalId rebuttal{};
};

struct Rebuttal {
    RebuttalId id{};
    ChallengeId challenge{};
    ChallengeGeneration challenge_generation{};
    FindingId finding{};
    CriticId author{};
    CriticGeneration author_generation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    std::string argument;
    std::vector<EvidenceId> evidence;
    std::uint64_t creation_order = 0;
};

struct SubmitChallengeRequest {
    FindingId challenged_finding{};
    FindingGeneration challenged_finding_generation{};
    std::string disputed_predicate;
    std::string rationale;
    std::vector<EvidenceId> evidence;
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

struct SubmitRebuttalRequest {
    ChallengeId challenge{};
    ChallengeGeneration challenge_generation{};
    ChallengeOutcome outcome = ChallengeOutcome::Unresolved;
    std::string argument;
    std::vector<EvidenceId> evidence;
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

#endif  // CRITIC_FABRIC_CHALLENGE_HPP

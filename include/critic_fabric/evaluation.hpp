// Critic Fabric — hard predicate evaluation and deterministic aggregation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_EVALUATION_HPP
#define CRITIC_FABRIC_EVALUATION_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "critic_fabric/authority.hpp"
#include "critic_fabric/challenge.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/evidence.hpp"
#include "critic_fabric/explanation.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/policy.hpp"
#include "critic_fabric/result.hpp"
#include "critic_fabric/review.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

// A detached view of live process authority. Evaluation never consults mutable
// coordinator internals, so a decision is a pure function of this view plus the
// durable review state.
struct LiveAuthorityView {
    CoordinatorEpoch epoch{};
    TargetId current_target{};
    TargetGeneration current_target_generation{};
    Digest current_target_digest{};
    std::map<CriticId, CriticGeneration> critic_generations;
    std::map<CriticId, WorkerIncarnation> incumbent_workers;
};

struct EvaluationInput {
    ReviewRecord review;
    std::vector<CriticAssignment> assignments;
    std::vector<Finding> findings;
    std::vector<EvidenceRecord> evidences;
    std::vector<Challenge> challenges;
    std::vector<Rebuttal> rebuttals;
    LiveAuthorityView authority;
    bool cancelled = false;
    bool superseded = false;
    // Canonical digest of the review policy. Supplied by the coordinator, which
    // computes it once per review generation instead of re-encoding the whole
    // specification on every evaluation. A zero digest means "not supplied" and
    // is computed here.
    Digest policy_digest{};
};

struct EvaluationResult {
    ReviewDisposition disposition = ReviewDisposition::None;
    Explanation explanation;
    // True when a mandatory hard predicate failed. A hard-invalid review can
    // never be reported as PASS no matter how favorable the aggregate is.
    bool hard_invalid = false;
    ErrorCode primary_rejection = ErrorCode::Ok;
};

// Deterministic: identical authoritative inputs always produce an identical
// disposition, predicate ordering, contributor ordering, and explanation digest.
EvaluationResult evaluate_review(const EvaluationInput& input);

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_EVALUATION_HPP

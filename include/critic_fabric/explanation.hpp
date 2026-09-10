// Critic Fabric — deterministic, structured review explanations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_EXPLANATION_HPP
#define CRITIC_FABRIC_EXPLANATION_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "critic_fabric/digest.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"

namespace critic_fabric {

struct PredicateEvaluation {
    PredicateKind kind = PredicateKind::ExactTargetGenerationMatch;
    bool mandatory = true;
    bool satisfied = false;
    std::uint32_t parameter = 0;
    std::string detail;
};

struct CriticContribution {
    CriticId critic{};
    CriticGeneration critic_generation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    CriticRole role = CriticRole::GeneralCritic;
    bool required = false;
    bool decisive = false;
    FindingId finding{};
    FindingOutcome outcome = FindingOutcome::Unknown;
    Severity severity = Severity::Info;
    std::uint32_t weight = 1;
    std::string note;
};

struct QuorumReport {
    std::uint32_t min_critic_count = 0;
    std::uint32_t required_critic_count = 0;
    std::uint32_t quorum = 0;
    std::uint32_t distinct_decisive_critics = 0;
    std::uint32_t distinct_abstaining_critics = 0;
    std::uint32_t distinct_unknown_critics = 0;
    std::uint32_t required_roles_decisive = 0;
    std::vector<CriticRole> uncovered_required_roles;
    bool satisfied = false;
    std::string basis;
};

struct DisagreementReport {
    bool present = false;
    std::vector<CriticId> pass_critics;
    std::vector<CriticId> fail_critics;
    std::vector<CriticId> warn_critics;
    std::vector<CriticId> abstaining_critics;
    std::vector<CriticId> unknown_critics;
    std::vector<CriticId> inconclusive_critics;
    std::vector<CriticId> missing_critics;
    std::vector<CriticId> failed_critics;
    std::vector<std::string> conflicting_pairs;
    std::uint32_t disagreeing_critic_count = 0;
    std::uint32_t policy_threshold = 0;
    ReviewDisposition disposition = ReviewDisposition::None;
};

struct Explanation {
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    Digest target_digest{};
    std::uint32_t target_generation_value = 0;
    ReviewState state = ReviewState::Declared;
    ReviewDisposition disposition = ReviewDisposition::None;
    CoordinatorEpoch epoch{};

    std::vector<CriticContribution> contributors;
    std::vector<PredicateEvaluation> predicates;
    QuorumReport quorum;
    DisagreementReport disagreement;

    std::vector<FindingId> current_findings;
    std::vector<FindingId> superseded_findings;
    std::vector<FindingId> invalidated_findings;
    std::vector<FindingId> excluded_findings;
    std::map<FindingId, std::string> exclusion_reasons;
    std::vector<EvidenceId> evidence_used;

    std::uint32_t pass_count = 0;
    std::uint32_t fail_count = 0;
    std::uint32_t warn_count = 0;
    std::uint32_t abstain_count = 0;
    std::uint32_t unknown_count = 0;
    std::uint32_t not_applicable_count = 0;
    std::uint32_t inconclusive_count = 0;
    Severity max_severity = Severity::Info;
    std::uint32_t critical_fail_count = 0;

    std::vector<ChallengeId> challenges;
    std::vector<ChallengeId> unresolved_challenges;

    Digest policy_digest{};
    Digest explanation_digest{};
    DecisionId decision{};
    DecisionGeneration decision_generation{};
    std::string policy_result;
    std::vector<std::string> notes;

    // Stable multi-line rendering. Field order, section order, and within-section
    // ordering are fixed; iteration over unordered containers is never a source
    // of authority here.
    [[nodiscard]] std::string render() const;
};

// Canonical digest over the fields that define the explanation's meaning. Two
// runs over identical authoritative inputs must produce identical digests.
Digest compute_explanation_digest(const Explanation& explanation);

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_EXPLANATION_HPP

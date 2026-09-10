// Critic Fabric — typed review policy specification.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_POLICY_HPP
#define CRITIC_FABRIC_POLICY_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "critic_fabric/digest.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"

namespace critic_fabric {

// One hard eligibility predicate. Mandatory predicates are evaluated before any
// aggregation; a favorable score can never rescue a failed mandatory one.
// Predicates marked authoritative_capable are the ones that can end a review on
// their own regardless of the remaining evidence.
struct PredicateSpec {
    PredicateKind kind = PredicateKind::ExactTargetGenerationMatch;
    bool mandatory = true;
    std::uint32_t parameter = 0;
};

// One row of a policy table. Rows are evaluated in declaration order and the
// first matching row wins, so the result is deterministic.
struct PolicyTableEntry {
    FindingOutcome outcome = FindingOutcome::Fail;
    Severity min_severity = Severity::Info;
    Severity max_severity = Severity::Critical;
    std::uint32_t min_count = 1;
    ReviewDisposition disposition = ReviewDisposition::Fail;
};

// The typed review specification. This is a configuration model, not a DSL.
struct ReviewSpec {
    std::string name = "default";

    // Required roles must each be covered by at least one authoritative,
    // decisive critic before a PASS-class disposition is possible.
    std::vector<CriticRole> required_roles;
    std::vector<CriticRole> optional_roles;
    std::set<CriticRole> roles_that_must_pass;

    // Distinct logical critic identities that must contribute a decisive,
    // current finding. Abstentions never count toward this denominator.
    std::uint32_t min_critic_count = 1;
    // Required-role critics that must report decisively.
    std::uint32_t required_critic_count = 1;
    // Distinct decisive contributors required to declare the review complete.
    std::uint32_t quorum = 1;

    std::map<FindingCategory, std::uint32_t> required_min_categories;
    std::vector<FindingCategory> required_finding_categories;

    std::uint32_t max_critical_fails = 0;
    bool allow_abstention = true;
    bool allow_partial_failure = false;

    // Severity thresholds. FAIL at or above fail_threshold is decisive for FAIL.
    Severity fail_threshold = Severity::Critical;
    Severity warn_threshold = Severity::Medium;
    bool warn_counts_as_failure = false;
    bool unknown_counts_as_failure = false;

    AggregationPolicy aggregation = AggregationPolicy::AllRequiredPass;
    DisagreementPolicy disagreement = DisagreementPolicy::ReportAndFail;
    std::uint32_t disagreement_threshold_fail = 1;

    DuplicatePolicy duplicates = DuplicatePolicy::Reject;
    ChallengePolicy challenges = ChallengePolicy::AllowOneRound;
    std::uint32_t max_challenge_rounds = 1;
    std::uint32_t max_review_rounds = 3;
    std::uint32_t max_critic_retries = 1;

    // Weights participate only in WeightedReview aggregation, and never in a
    // hard predicate. A critic without a weight has weight 1.
    std::map<CriticId, std::uint32_t> critic_weights;
    std::uint32_t default_weight = 1;

    std::vector<PredicateSpec> predicates;
    std::vector<PolicyTableEntry> policy_table;

    // ConsensusThreshold aggregation: percentage of decisive critics (in basis
    // points, 0..10000) that must agree on the winning disposition.
    std::uint32_t consensus_threshold_basis_points = 6700;

    // Reserved and deliberately fixed: confidence is advisory metadata and is
    // never granted decision authority. Attempting to enable this is rejected.
    bool confidence_has_authority = false;

    bool deterministic_decision_required = true;
    bool persist_findings = true;
    bool persist_evidence_metadata = true;
    bool persist_evidence_payload = false;
    bool reopen_on_recovery = false;

    [[nodiscard]] std::uint32_t weight_of(CriticId critic) const;
    [[nodiscard]] bool is_required_role(CriticRole role) const;
    [[nodiscard]] PredicateSpec effective_predicate(PredicateKind kind) const;
    [[nodiscard]] Digest compute_policy_digest() const;
    [[nodiscard]] std::string summarize() const;
};

// A locally reasonable default: one decisive mandatory critic, strict
// no-silent-pass semantics, one bounded challenge round.
ReviewSpec default_review_spec();

// Validates internal consistency (for example quorum above min_critic_count, or
// an empty required-role list with required_critic_count above zero). Returns a
// typed rejection describing the first inconsistency found.
class Status;
Status validate_review_spec(const ReviewSpec& spec);

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_POLICY_HPP

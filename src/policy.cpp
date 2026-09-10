// Critic Fabric — review policy semantics.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/policy.hpp"

#include <algorithm>

#include "critic_fabric/digest_domain.hpp"
#include "critic_fabric/limits.hpp"
#include "critic_fabric/result.hpp"
#include "critic_fabric/serialization.hpp"

namespace critic_fabric {

std::uint32_t ReviewSpec::weight_of(CriticId critic) const {
    const auto it = critic_weights.find(critic);
    if (it == critic_weights.end()) {
        return default_weight == 0 ? 1u : default_weight;
    }
    return it->second == 0 ? 1u : it->second;
}

bool ReviewSpec::is_required_role(CriticRole role) const {
    return std::find(required_roles.begin(), required_roles.end(), role) != required_roles.end();
}

PredicateSpec ReviewSpec::effective_predicate(PredicateKind kind) const {
    for (const PredicateSpec& predicate : predicates) {
        if (predicate.kind == kind) {
            return predicate;
        }
    }
    PredicateSpec absent;
    absent.kind = kind;
    absent.mandatory = false;
    absent.parameter = 0;
    return absent;
}

Digest ReviewSpec::compute_policy_digest() const {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::Policy));
    encode(writer, *this);
    return Sha256::hash(writer.buffer());
}

std::string ReviewSpec::summarize() const {
    std::string out;
    out.reserve(256);
    out += "spec=";
    out += name;
    out += " aggregation=";
    out += to_string(aggregation);
    out += " quorum=";
    out += std::to_string(quorum);
    out += " min_critics=";
    out += std::to_string(min_critic_count);
    out += " required_critics=";
    out += std::to_string(required_critic_count);
    out += " required_roles=[";
    for (std::size_t i = 0; i < required_roles.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += to_string(required_roles[i]);
    }
    out += "] fail_threshold=";
    out += to_string(fail_threshold);
    out += " unknown_counts_as_failure=";
    out += unknown_counts_as_failure ? "true" : "false";
    out += " disagreement=";
    out += to_string(disagreement);
    out += " duplicates=";
    out += to_string(duplicates);
    out += " confidence_authority=false";
    return out;
}

ReviewSpec default_review_spec() {
    ReviewSpec spec;
    spec.name = "critic-fabric-default";
    spec.required_roles = {CriticRole::GeneralCritic};
    spec.optional_roles = {};
    spec.roles_that_must_pass = {};
    spec.min_critic_count = 1;
    spec.required_critic_count = 1;
    spec.quorum = 1;
    spec.max_critical_fails = 0;
    spec.allow_abstention = true;
    spec.allow_partial_failure = false;
    spec.fail_threshold = Severity::Critical;
    spec.warn_threshold = Severity::Medium;
    spec.warn_counts_as_failure = false;
    spec.unknown_counts_as_failure = false;
    spec.aggregation = AggregationPolicy::AllRequiredPass;
    spec.disagreement = DisagreementPolicy::ReportAndFail;
    spec.disagreement_threshold_fail = 1;
    spec.duplicates = DuplicatePolicy::Reject;
    spec.challenges = ChallengePolicy::AllowOneRound;
    spec.max_challenge_rounds = 1;
    spec.max_review_rounds = 3;
    spec.max_critic_retries = 1;
    spec.predicates = {
        {PredicateKind::ExactTargetGenerationMatch, true, 0},
        {PredicateKind::TargetDigestMatch, true, 0},
        {PredicateKind::RequiredEvidencePresent, true, 1},
        {PredicateKind::EvidenceIntegrityVerified, true, 0},
        {PredicateKind::EvidenceProvenanceKnown, true, 0},
        {PredicateKind::RequiredRolePresent, true, 0},
        {PredicateKind::MinimumIndependentCriticIdentities, true, 1},
        {PredicateKind::RequiredVerifierPass, true, 0},
        {PredicateKind::NoCriticalFailFindings, true, 0},
        {PredicateKind::CriticGenerationCurrent, true, 0},
        {PredicateKind::WorkerBootCurrent, true, 0},
        {PredicateKind::CoordinatorEpochCurrent, true, 0},
        {PredicateKind::RequiredCategoriesCovered, true, 0},
        {PredicateKind::QuorumSatisfied, true, 0},
        {PredicateKind::ConfidenceNotAuthoritative, true, 0},
    };
    return spec;
}

Status validate_review_spec(const ReviewSpec& spec) {
    if (spec.name.empty()) {
        return Status::error(ErrorCode::PolicyRejected, "review specification name is empty");
    }
    if (spec.name.size() > kHardMaxMetadataBytes) {
        return Status::error(ErrorCode::PolicyRejected, "review specification name exceeds the metadata bound");
    }
    if (spec.min_critic_count == 0) {
        return Status::error(ErrorCode::PolicyRejected, "min_critic_count must be at least one");
    }
    if (spec.required_critic_count > spec.min_critic_count) {
        return Status::error(ErrorCode::PolicyRejected,
                             "required_critic_count exceeds min_critic_count; required critics are a subset "
                             "of the participating critics");
    }
    if (spec.quorum > spec.min_critic_count) {
        return Status::error(ErrorCode::PolicyRejected,
                             "quorum exceeds min_critic_count; a quorum larger than the participation floor can "
                             "never be met");
    }
    if (spec.required_critic_count > 0 && spec.required_roles.empty() && spec.required_finding_categories.empty() &&
        spec.required_min_categories.empty()) {
        return Status::error(ErrorCode::PolicyRejected,
                             "required_critic_count is positive but no required role or category is declared");
    }
    if (spec.max_review_rounds == 0) {
        return Status::error(ErrorCode::PolicyRejected, "max_review_rounds must be at least one");
    }
    if (spec.max_review_rounds > kHardMaxReviewRounds) {
        return Status::error(ErrorCode::PolicyRejected, "max_review_rounds exceeds the hard ceiling");
    }
    if (spec.max_critic_retries > kHardMaxRetries) {
        return Status::error(ErrorCode::PolicyRejected, "max_critic_retries exceeds the hard ceiling");
    }
    if (spec.challenges == ChallengePolicy::Disabled && spec.max_challenge_rounds != 0) {
        return Status::error(ErrorCode::PolicyRejected,
                             "challenge policy is disabled but max_challenge_rounds is non-zero");
    }
    if (spec.challenges == ChallengePolicy::AllowOneRound && spec.max_challenge_rounds != 1) {
        return Status::error(ErrorCode::PolicyRejected,
                             "AllowOneRound challenge policy requires max_challenge_rounds of exactly one");
    }
    if (spec.challenges == ChallengePolicy::AllowBoundedRounds && spec.max_challenge_rounds == 0) {
        return Status::error(ErrorCode::PolicyRejected,
                             "AllowBoundedRounds challenge policy requires a non-zero round bound");
    }
    if (spec.challenges != ChallengePolicy::AllowBoundedRounds && spec.max_challenge_rounds > 1) {
        return Status::error(ErrorCode::PolicyRejected,
                             "max_challenge_rounds above one requires the AllowBoundedRounds challenge policy");
    }
    if (spec.confidence_has_authority) {
        return Status::error(ErrorCode::PolicyRejected,
                             "confidence may not be granted decision authority; confidence is advisory metadata");
    }
    if (spec.consensus_threshold_basis_points > 10000u) {
        return Status::error(ErrorCode::PolicyRejected, "consensus threshold outside 0..10000 basis points");
    }
    if (spec.aggregation == AggregationPolicy::ConsensusThreshold &&
        spec.consensus_threshold_basis_points == 0) {
        return Status::error(ErrorCode::PolicyRejected,
                             "ConsensusThreshold aggregation requires a non-zero consensus threshold");
    }
    if (spec.aggregation == AggregationPolicy::PolicyTable && spec.policy_table.empty()) {
        return Status::error(ErrorCode::PolicyRejected,
                             "PolicyTable aggregation requires at least one policy table row");
    }
    if (spec.default_weight == 0) {
        return Status::error(ErrorCode::PolicyRejected,
                             "default_weight must be at least one so that a missing weight cannot silently "
                             "erase a critic from a weighted review");
    }
    for (const auto& entry : spec.critic_weights) {
        if (!entry.first.valid()) {
            return Status::error(ErrorCode::PolicyRejected, "critic weight table contains an invalid critic identity");
        }
        if (entry.second == 0) {
            return Status::error(ErrorCode::PolicyRejected,
                                 "a zero critic weight would silently erase a critic from a weighted review");
        }
    }
    for (const PredicateSpec& predicate : spec.predicates) {
        if (!is_valid_predicate_kind(static_cast<std::uint32_t>(predicate.kind))) {
            return Status::error(ErrorCode::PolicyRejected, "predicate list contains an unknown predicate kind");
        }
    }
    for (const PolicyTableEntry& entry : spec.policy_table) {
        if (!is_valid_finding_outcome(static_cast<std::uint32_t>(entry.outcome))) {
            return Status::error(ErrorCode::PolicyRejected, "policy table row carries an invalid outcome");
        }
        if (severity_rank(entry.min_severity) > severity_rank(entry.max_severity)) {
            return Status::error(ErrorCode::PolicyRejected,
                                 "policy table row declares a severity range with min above max");
        }
        if (entry.min_count == 0) {
            return Status::error(ErrorCode::PolicyRejected, "policy table row declares a zero minimum count");
        }
    }
    if (spec.predicates.size() > 64) {
        return Status::error(ErrorCode::PolicyRejected, "too many predicates declared");
    }
    return Status::success();
}

}  // namespace critic_fabric

// Critic Fabric — hard predicate evaluation and deterministic aggregation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/evaluation.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace critic_fabric {
namespace {

struct CriticVote {
    CriticId critic{};
    CriticGeneration critic_generation{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    CriticRole role = CriticRole::GeneralCritic;
    bool required = false;
    bool assigned = false;
    std::uint32_t weight = 1;
    std::vector<FindingId> findings;
    std::uint32_t pass_count = 0;
    std::uint32_t fail_count = 0;
    std::uint32_t warn_count = 0;
    std::uint32_t fail_at_threshold = 0;
    bool conflicted = false;
    FindingOutcome dominant = FindingOutcome::Unknown;
    Severity max_severity = Severity::Info;
    bool decisive = false;
    bool abstained = false;
    bool unknown = false;
    bool inconclusive = false;
    bool present = false;
};

std::string finding_reference(FindingId id) {
    return "#" + std::to_string(id.value());
}

bool same_incarnation(const WorkerIncarnation& a, WorkerId worker, WorkerBootId boot) {
    return a.id == worker && a.boot == boot;
}

bool evidence_is_usable(const EvidenceRecord& evidence) {
    return evidence.current && !evidence.superseded && evidence.integrity == EvidenceIntegrity::Verified &&
           evidence.provenance != EvidenceProvenance::Unknown;
}

}  // namespace

EvaluationResult evaluate_review(const EvaluationInput& input) {
    EvaluationResult result;
    const ReviewSpec& spec = input.review.spec;

    Explanation& ex = result.explanation;
    ex.review = input.review.id;
    ex.review_generation = input.review.generation;
    ex.target = input.review.target;
    ex.target_generation = input.review.target_generation;
    ex.target_digest = input.review.target_digest;
    ex.target_generation_value = static_cast<std::uint32_t>(input.review.target_generation.value());
    ex.state = input.review.state;
    ex.epoch = input.authority.epoch;
    ex.policy_digest = input.policy_digest.is_zero() ? spec.compute_policy_digest() : input.policy_digest;
    ex.policy_result = spec.summarize();

    // --- 1. Classify findings against current authority ---------------------
    std::map<FindingId, const Finding*> current;
    for (const Finding& finding : input.findings) {
        const char* reason = nullptr;
        if (finding.review != input.review.id) {
            reason = "finding belongs to a different review";
        } else if (finding.review_generation != input.review.generation) {
            reason = "stale review generation";
        } else if (finding.target != input.review.target) {
            reason = "finding targets a different target";
        } else if (finding.target_generation != input.review.target_generation) {
            reason = "stale target generation";
        } else if (finding.state != FindingState::Current) {
            reason = "finding is not current";
        } else {
            const auto generation_it = input.authority.critic_generations.find(finding.critic);
            if (generation_it == input.authority.critic_generations.end()) {
                reason = "critic is not registered under current authority";
            } else if (generation_it->second != finding.critic_generation) {
                reason = "stale critic generation";
            } else {
                const auto worker_it = input.authority.incumbent_workers.find(finding.critic);
                if (worker_it == input.authority.incumbent_workers.end()) {
                    reason = "critic has no authoritative worker incarnation";
                } else if (!same_incarnation(worker_it->second, finding.worker, finding.worker_boot)) {
                    reason = "stale worker boot";
                }
            }
        }
        if (reason != nullptr) {
            ex.excluded_findings.push_back(finding.id);
            ex.exclusion_reasons[finding.id] = reason;
            continue;
        }
        current[finding.id] = &finding;
    }

    for (const Finding& finding : input.findings) {
        switch (finding.state) {
            case FindingState::Superseded:
                ex.superseded_findings.push_back(finding.id);
                break;
            case FindingState::Invalidated:
            case FindingState::Withdrawn:
                ex.invalidated_findings.push_back(finding.id);
                break;
            default:
                break;
        }
    }

    // --- 2. Collapse current findings into one vote per logical critic ------
    std::map<CriticId, CriticVote> votes;
    for (const auto& entry : current) {
        const Finding& finding = *entry.second;
        CriticVote& vote = votes[finding.critic];
        vote.critic = finding.critic;
        vote.critic_generation = finding.critic_generation;
        vote.worker = finding.worker;
        vote.worker_boot = finding.worker_boot;
        vote.present = true;
        vote.findings.push_back(finding.id);
        if (severity_rank(finding.severity) > severity_rank(vote.max_severity)) {
            vote.max_severity = finding.severity;
        }
        switch (finding.outcome) {
            case FindingOutcome::Pass:
                vote.pass_count += 1;
                break;
            case FindingOutcome::Fail:
                vote.fail_count += 1;
                if (severity_rank(finding.severity) >= severity_rank(spec.fail_threshold)) {
                    vote.fail_at_threshold += 1;
                }
                break;
            case FindingOutcome::Warn:
                vote.warn_count += 1;
                break;
            case FindingOutcome::Abstain:
                vote.abstained = true;
                break;
            case FindingOutcome::Unknown:
                vote.unknown = true;
                break;
            case FindingOutcome::Inconclusive:
                vote.inconclusive = true;
                break;
            case FindingOutcome::NotApplicable:
                break;
        }
        if (vote.pass_count > 0 && vote.fail_count > 0) {
            // One logical critic may not assert both PASS and FAIL for the same
            // target generation. The coordinator rejects this at ingest; if it
            // is ever observed here the critic contributes no vote.
            vote.conflicted = true;
        }
    }

    // Attach assignment metadata and record assignments that produced nothing.
    for (const CriticAssignment& assignment : input.assignments) {
        CriticVote& vote = votes[assignment.critic];
        vote.critic = assignment.critic;
        vote.critic_generation = assignment.critic_generation;
        vote.worker = assignment.worker;
        vote.worker_boot = assignment.worker_boot;
        vote.role = assignment.role;
        vote.required = assignment.required;
        vote.assigned = assignment.active;
        vote.weight = assignment.weight == 0 ? 1u : assignment.weight;
    }

    for (auto& entry : votes) {
        CriticVote& vote = entry.second;
        if (vote.conflicted) {
            vote.dominant = FindingOutcome::Unknown;
            vote.decisive = false;
        } else if (vote.fail_count > 0) {
            vote.dominant = FindingOutcome::Fail;
            vote.decisive = true;
        } else if (vote.warn_count > 0) {
            vote.dominant = FindingOutcome::Warn;
            vote.decisive = true;
        } else if (vote.pass_count > 0) {
            vote.dominant = FindingOutcome::Pass;
            vote.decisive = true;
        } else {
            vote.dominant = FindingOutcome::Unknown;
            vote.decisive = false;
        }
    }

    // --- 3. Contributors, in deterministic assignment-then-critic order -----
    for (const auto& entry : votes) {
        const CriticVote& vote = entry.second;
        CriticContribution contribution;
        contribution.critic = vote.critic;
        contribution.critic_generation = vote.critic_generation;
        contribution.worker = vote.worker;
        contribution.worker_boot = vote.worker_boot;
        contribution.role = vote.role;
        contribution.required = vote.required;
        contribution.decisive = vote.decisive;
        contribution.finding = vote.findings.empty() ? FindingId{} : vote.findings.front();
        contribution.outcome = vote.dominant;
        contribution.severity = vote.max_severity;
        contribution.weight = vote.weight;
        if (vote.conflicted) {
            contribution.note = "critic asserted both PASS and FAIL for the same target generation";
        } else if (!vote.present) {
            contribution.note = "no current authoritative finding";
        } else if (!vote.decisive) {
            contribution.note = "no decisive current finding";
        }
        ex.contributors.push_back(contribution);
    }

    // --- 4. Evidence actually in play ---------------------------------------
    std::set<EvidenceId> referenced;
    for (const auto& entry : current) {
        for (EvidenceId id : entry.second->evidence) {
            referenced.insert(id);
        }
    }
    std::map<EvidenceId, const EvidenceRecord*> evidence_index;
    for (const EvidenceRecord& evidence : input.evidences) {
        evidence_index[evidence.id] = &evidence;
    }
    std::uint32_t usable_evidence = 0;
    std::uint32_t unverified_evidence = 0;
    std::uint32_t unknown_provenance_evidence = 0;
    std::uint32_t missing_evidence = 0;
    for (EvidenceId id : referenced) {
        const auto it = evidence_index.find(id);
        if (it == evidence_index.end()) {
            missing_evidence += 1;
            continue;
        }
        const EvidenceRecord& evidence = *it->second;
        ex.evidence_used.push_back(id);
        if (evidence_is_usable(evidence)) {
            usable_evidence += 1;
        } else {
            if (evidence.integrity != EvidenceIntegrity::Verified) {
                unverified_evidence += 1;
            }
            if (evidence.provenance == EvidenceProvenance::Unknown) {
                unknown_provenance_evidence += 1;
            }
        }
    }

    // --- 5. Quorum and disagreement -----------------------------------------
    std::vector<const CriticVote*> decisive;
    std::vector<const CriticVote*> abstaining;
    std::vector<const CriticVote*> unknown_votes;
    std::vector<const CriticVote*> inconclusive_votes;
    std::vector<const CriticVote*> missing;
    std::vector<const CriticVote*> failed;
    for (const auto& entry : votes) {
        const CriticVote& vote = entry.second;
        if (!vote.assigned) {
            continue;
        }
        if (vote.decisive) {
            decisive.push_back(&vote);
        } else if (vote.conflicted) {
            failed.push_back(&vote);
        } else if (vote.abstained) {
            abstaining.push_back(&vote);
        } else if (vote.unknown) {
            unknown_votes.push_back(&vote);
        } else if (vote.inconclusive) {
            inconclusive_votes.push_back(&vote);
        } else {
            const auto generation_it = input.authority.critic_generations.find(vote.critic);
            const auto worker_it = input.authority.incumbent_workers.find(vote.critic);
            const bool authoritative =
                generation_it != input.authority.critic_generations.end() &&
                generation_it->second == vote.critic_generation &&
                worker_it != input.authority.incumbent_workers.end() &&
                same_incarnation(worker_it->second, vote.worker, vote.worker_boot);
            if (authoritative) {
                missing.push_back(&vote);
            } else {
                failed.push_back(&vote);
            }
        }
    }

    QuorumReport& quorum = ex.quorum;
    quorum.min_critic_count = spec.min_critic_count;
    quorum.required_critic_count = spec.required_critic_count;
    quorum.quorum = spec.quorum;
    quorum.distinct_decisive_critics = static_cast<std::uint32_t>(decisive.size());
    quorum.distinct_abstaining_critics = static_cast<std::uint32_t>(abstaining.size());
    quorum.distinct_unknown_critics = static_cast<std::uint32_t>(unknown_votes.size());

    std::uint32_t required_roles_decisive = 0;
    for (CriticRole role : spec.required_roles) {
        const bool covered = std::any_of(decisive.begin(), decisive.end(),
                                         [role](const CriticVote* vote) { return vote->role == role; });
        if (covered) {
            required_roles_decisive += 1;
        } else {
            quorum.uncovered_required_roles.push_back(role);
        }
    }
    quorum.required_roles_decisive = required_roles_decisive;
    quorum.satisfied = quorum.distinct_decisive_critics >= spec.quorum &&
                       quorum.distinct_decisive_critics >= spec.min_critic_count &&
                       required_roles_decisive >= spec.required_critic_count &&
                       quorum.uncovered_required_roles.empty();
    quorum.basis = "distinct decisive critics " + std::to_string(quorum.distinct_decisive_critics) + "/" +
                   std::to_string(spec.min_critic_count) + ", quorum " + std::to_string(spec.quorum) +
                   ", required roles covered " + std::to_string(required_roles_decisive) + "/" +
                   std::to_string(spec.required_critic_count) + ", abstaining " +
                   std::to_string(quorum.distinct_abstaining_critics);

    DisagreementReport& disagreement = ex.disagreement;
    disagreement.policy_threshold = spec.disagreement_threshold_fail;
    std::map<FindingOutcome, std::vector<CriticId>> by_outcome;
    for (const CriticVote* vote : decisive) {
        by_outcome[vote->dominant].push_back(vote->critic);
        switch (vote->dominant) {
            case FindingOutcome::Pass:
                disagreement.pass_critics.push_back(vote->critic);
                break;
            case FindingOutcome::Fail:
                disagreement.fail_critics.push_back(vote->critic);
                break;
            case FindingOutcome::Warn:
                disagreement.warn_critics.push_back(vote->critic);
                break;
            default:
                break;
        }
    }
    for (const CriticVote* vote : abstaining) {
        disagreement.abstaining_critics.push_back(vote->critic);
    }
    for (const CriticVote* vote : unknown_votes) {
        disagreement.unknown_critics.push_back(vote->critic);
    }
    for (const CriticVote* vote : inconclusive_votes) {
        disagreement.inconclusive_critics.push_back(vote->critic);
    }
    for (const CriticVote* vote : missing) {
        disagreement.missing_critics.push_back(vote->critic);
    }
    for (const CriticVote* vote : failed) {
        disagreement.failed_critics.push_back(vote->critic);
    }

    const std::string labels[] = {"PASS", "FAIL", "WARN"};
    const FindingOutcome outcomes[] = {FindingOutcome::Pass, FindingOutcome::Fail, FindingOutcome::Warn};
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            const auto& left = by_outcome[outcomes[i]];
            const auto& right = by_outcome[outcomes[j]];
            if (!left.empty() && !right.empty()) {
                disagreement.conflicting_pairs.push_back(labels[i] + " x" + std::to_string(left.size()) + " vs " +
                                                         labels[j] + " x" + std::to_string(right.size()));
            }
        }
    }

    if (!decisive.empty()) {
        std::size_t best = 0;
        bool tie = false;
        for (const auto& entry : by_outcome) {
            if (entry.second.size() > best) {
                best = entry.second.size();
                tie = false;
            } else if (entry.second.size() == best) {
                tie = true;
            }
        }
        if (by_outcome.size() <= 1) {
            disagreement.disagreeing_critic_count = 0;
        } else if (tie) {
            disagreement.disagreeing_critic_count = static_cast<std::uint32_t>(decisive.size());
        } else {
            disagreement.disagreeing_critic_count = static_cast<std::uint32_t>(decisive.size() - best);
        }
    }
    disagreement.present = disagreement.disagreeing_critic_count > 0;

    // --- 5b. Challenge state -------------------------------------------------
    for (const Challenge& challenge : input.challenges) {
        ex.challenges.push_back(challenge.id);
        if (!challenge.resolved || challenge.outcome == ChallengeOutcome::Unresolved) {
            ex.unresolved_challenges.push_back(challenge.id);
        }
    }

    // --- 6. Hard predicates, evaluated before any scoring -------------------
    std::uint32_t critical_fail_count = 0;
    std::uint32_t fail_count = 0;
    std::uint32_t warn_count = 0;
    std::uint32_t pass_count = 0;
    for (const auto& entry : current) {
        const Finding& finding = *entry.second;
        switch (finding.outcome) {
            case FindingOutcome::Pass:
                pass_count += 1;
                break;
            case FindingOutcome::Fail:
                fail_count += 1;
                if (severity_rank(finding.severity) >= severity_rank(spec.fail_threshold)) {
                    critical_fail_count += 1;
                }
                break;
            case FindingOutcome::Warn:
                warn_count += 1;
                break;
            case FindingOutcome::Abstain:
                ex.abstain_count += 1;
                break;
            case FindingOutcome::Unknown:
                ex.unknown_count += 1;
                break;
            case FindingOutcome::NotApplicable:
                ex.not_applicable_count += 1;
                break;
            case FindingOutcome::Inconclusive:
                ex.inconclusive_count += 1;
                break;
        }
    }
    ex.pass_count = pass_count;
    ex.fail_count = fail_count;
    ex.warn_count = warn_count;
    ex.critical_fail_count = critical_fail_count;
    for (const auto& entry : current) {
        ex.current_findings.push_back(entry.first);
    }
    for (const auto& entry : votes) {
        if (severity_rank(entry.second.max_severity) > severity_rank(ex.max_severity)) {
            ex.max_severity = entry.second.max_severity;
        }
    }
    bool hard_invalid = false;
    ErrorCode primary_rejection = ErrorCode::Ok;

    auto record = [&ex](PredicateKind kind, bool mandatory, bool satisfied, std::uint32_t parameter,
                        std::string detail) {
        PredicateEvaluation evaluation;
        evaluation.kind = kind;
        evaluation.mandatory = mandatory;
        evaluation.satisfied = satisfied;
        evaluation.parameter = parameter;
        evaluation.detail = std::move(detail);
        ex.predicates.push_back(std::move(evaluation));
    };
    auto fail_predicate = [&hard_invalid, &primary_rejection](bool mandatory, ErrorCode code) {
        if (mandatory) {
            hard_invalid = true;
            if (primary_rejection == ErrorCode::Ok) {
                primary_rejection = code;
            }
        }
    };

    // Predicates are evaluated in the declared order, which is the order they
    // appear in the specification; the list is part of the policy digest.
    for (const PredicateSpec& predicate : spec.predicates) {
        const bool mandatory = predicate.mandatory;
        bool satisfied = false;
        std::string detail;
        ErrorCode code = ErrorCode::HardPredicateFailed;
        switch (predicate.kind) {
            case PredicateKind::ExactTargetGenerationMatch: {
                const bool generation_matches =
                    input.review.target == input.authority.current_target &&
                    (input.authority.current_target_generation.value() == 0 ||
                     input.review.target_generation == input.authority.current_target_generation);
                bool all_findings_bound = true;
                for (const Finding& finding : input.findings) {
                    if (finding.review == input.review.id &&
                        finding.review_generation == input.review.generation &&
                        finding.target_generation != input.review.target_generation) {
                        all_findings_bound = false;
                    }
                }
                satisfied = generation_matches && all_findings_bound;
                detail = satisfied ? "review is bound to the current target generation"
                                   : "review target generation no longer matches live target authority";
                code = ErrorCode::RevalidationRequired;
                break;
            }
            case PredicateKind::TargetDigestMatch: {
                satisfied = input.authority.current_target_digest.is_zero() ||
                            input.review.target_digest == input.authority.current_target_digest;
                detail = satisfied ? "review target digest matches live target content"
                                   : "review target digest no longer matches live target content";
                code = ErrorCode::TargetDigestMismatch;
                break;
            }
            case PredicateKind::RequiredEvidencePresent: {
                const std::uint32_t minimum =
                    predicate.parameter != 0 ? predicate.parameter : 1u;
                satisfied = usable_evidence >= minimum && missing_evidence == 0;
                detail = "usable evidence records " + std::to_string(usable_evidence) + " of required " +
                         std::to_string(minimum) + ", missing references " + std::to_string(missing_evidence);
                code = ErrorCode::InsufficientEvidence;
                break;
            }
            case PredicateKind::EvidenceIntegrityVerified: {
                satisfied = unverified_evidence == 0 && missing_evidence == 0;
                detail = "evidence with unverified or failed integrity: " + std::to_string(unverified_evidence);
                code = ErrorCode::EvidenceIntegrityFailed;
                break;
            }
            case PredicateKind::EvidenceProvenanceKnown: {
                satisfied = unknown_provenance_evidence == 0 && missing_evidence == 0;
                detail = "evidence with UNKNOWN provenance: " + std::to_string(unknown_provenance_evidence);
                code = ErrorCode::EvidenceProvenanceUnknown;
                break;
            }
            case PredicateKind::RequiredRolePresent: {
                satisfied = quorum.uncovered_required_roles.empty();
                detail = satisfied ? "all required roles produced a decisive finding"
                                   : "required roles without a decisive finding: " +
                                         std::to_string(quorum.uncovered_required_roles.size());
                code = ErrorCode::QuorumNotReached;
                break;
            }
            case PredicateKind::MinimumIndependentCriticIdentities: {
                const std::uint32_t minimum =
                    predicate.parameter != 0 ? predicate.parameter : spec.min_critic_count;
                satisfied = quorum.distinct_decisive_critics >= minimum;
                detail = "distinct decisive critic identities " +
                         std::to_string(quorum.distinct_decisive_critics) + " of required " +
                         std::to_string(minimum);
                code = ErrorCode::QuorumNotReached;
                break;
            }
            case PredicateKind::RequiredVerifierPass: {
                bool ok = true;
                std::string offenders;
                for (CriticRole role : spec.roles_that_must_pass) {
                    bool covered = false;
                    for (const CriticVote* vote : decisive) {
                        if (vote->role != role) {
                            continue;
                        }
                        covered = true;
                        if (vote->dominant == FindingOutcome::Fail) {
                            ok = false;
                            if (!offenders.empty()) {
                                offenders += ",";
                            }
                            offenders += to_string(role);
                        }
                    }
                    if (!covered) {
                        ok = false;
                        if (!offenders.empty()) {
                            offenders += ",";
                        }
                        offenders += std::string(to_string(role)) + "(no decisive finding)";
                    }
                }
                satisfied = ok;
                detail = satisfied ? "roles that must pass have all passed"
                                   : "roles that must pass did not: " + offenders;
                code = ErrorCode::RequiredCriticFailed;
                break;
            }
            case PredicateKind::NoCriticalFailFindings: {
                const std::uint32_t limit =
                    predicate.parameter != 0 ? predicate.parameter : spec.max_critical_fails;
                satisfied = critical_fail_count <= limit;
                detail = "findings at or above the fail threshold: " + std::to_string(critical_fail_count) +
                         " of allowed " + std::to_string(limit);
                code = ErrorCode::HardPredicateFailed;
                break;
            }
            case PredicateKind::CriticGenerationCurrent: {
                bool ok = true;
                for (const CriticAssignment& assignment : input.assignments) {
                    if (!assignment.active) {
                        continue;
                    }
                    const auto it = input.authority.critic_generations.find(assignment.critic);
                    if (it == input.authority.critic_generations.end() ||
                        it->second != assignment.critic_generation) {
                        ok = false;
                        break;
                    }
                }
                satisfied = ok;
                detail = satisfied ? "every active assignment references a current critic generation"
                                   : "an active assignment references a stale critic generation";
                code = ErrorCode::StaleCriticGeneration;
                break;
            }
            case PredicateKind::WorkerBootCurrent: {
                bool ok = true;
                for (const CriticAssignment& assignment : input.assignments) {
                    if (!assignment.active) {
                        continue;
                    }
                    const auto it = input.authority.incumbent_workers.find(assignment.critic);
                    if (it == input.authority.incumbent_workers.end() ||
                        !same_incarnation(it->second, assignment.worker, assignment.worker_boot)) {
                        ok = false;
                        break;
                    }
                }
                satisfied = ok;
                detail = satisfied ? "every active assignment references the incumbent worker boot"
                                   : "an active assignment references a superseded worker incarnation";
                code = ErrorCode::StaleWorkerBoot;
                break;
            }
            case PredicateKind::CoordinatorEpochCurrent: {
                satisfied = input.review.authority_current;
                detail = satisfied ? "review authority was established under the current coordinator epoch"
                                   : "review authority predates the current coordinator epoch";
                code = ErrorCode::RevalidationRequired;
                break;
            }
            case PredicateKind::RequiredCategoriesCovered: {
                bool ok = true;
                std::string missing_categories;
                for (const auto& entry : spec.required_min_categories) {
                    std::uint32_t found = 0;
                    for (const auto& finding_entry : current) {
                        if (finding_entry.second->category == entry.first &&
                            is_decisive_outcome(finding_entry.second->outcome)) {
                            found += 1;
                        }
                    }
                    if (found < entry.second) {
                        ok = false;
                        missing_categories += to_string(entry.first);
                        missing_categories += " ";
                    }
                }
                for (FindingCategory category : spec.required_finding_categories) {
                    const bool found = std::any_of(current.begin(), current.end(), [category](const auto& e) {
                        return e.second->category == category && is_decisive_outcome(e.second->outcome);
                    });
                    if (!found) {
                        ok = false;
                        missing_categories += to_string(category);
                        missing_categories += " ";
                    }
                }
                satisfied = ok;
                detail = satisfied ? "every required finding category is covered"
                                   : "required finding categories not covered: " + missing_categories;
                code = ErrorCode::InsufficientEvidence;
                break;
            }
            case PredicateKind::QuorumSatisfied: {
                satisfied = quorum.satisfied;
                detail = quorum.basis;
                code = ErrorCode::QuorumNotReached;
                break;
            }
            case PredicateKind::ConfidenceNotAuthoritative: {
                satisfied = !spec.confidence_has_authority;
                detail = satisfied ? "confidence is advisory metadata and carries no decision authority"
                                   : "policy attempted to grant confidence decision authority";
                code = ErrorCode::PolicyRejected;
                break;
            }
        }
        record(predicate.kind, mandatory, satisfied, predicate.parameter, detail);
        if (!satisfied) {
            fail_predicate(mandatory, code);
        }
    }

    if (!input.review.authority_current) {
        hard_invalid = true;
        if (primary_rejection == ErrorCode::Ok) {
            primary_rejection = ErrorCode::RevalidationRequired;
        }
    }

    // --- 7. Early exits that must precede aggregation -----------------------
    if (input.cancelled || input.review.state == ReviewState::Cancelled) {
        result.disposition = ReviewDisposition::Cancelled;
        ex.disposition = result.disposition;
        ex.notes.push_back("review was cancelled; no critic aggregate can produce a success outcome");
        ex.explanation_digest = compute_explanation_digest(ex);
        return result;
    }
    if (input.superseded || input.review.state == ReviewState::Superseded) {
        result.disposition = ReviewDisposition::Superseded;
        ex.disposition = result.disposition;
        ex.notes.push_back("review was superseded by a newer review generation");
        ex.explanation_digest = compute_explanation_digest(ex);
        return result;
    }
    if (input.review.state == ReviewState::RevalidationRequired) {
        result.disposition = ReviewDisposition::RevalidationRequired;
        ex.disposition = result.disposition;
        ex.notes.push_back("review requires revalidation before any disposition may be committed");
        ex.explanation_digest = compute_explanation_digest(ex);
        return result;
    }

    // --- 8. Aggregation ------------------------------------------------------
    const std::uint32_t denominator = static_cast<std::uint32_t>(decisive.size());
    ReviewDisposition aggregated = ReviewDisposition::InsufficientEvidence;
    std::string aggregation_basis;

    auto any_critical = [&]() { return critical_fail_count > spec.max_critical_fails; };
    auto all_required_fail = [&]() {
        return std::any_of(decisive.begin(), decisive.end(),
                           [&](const CriticVote* vote) {
                               return vote->required && vote->dominant == FindingOutcome::Fail &&
                                      vote->fail_at_threshold > 0;
                           });
    };
    auto all_required_pass = [&]() {
        return std::all_of(decisive.begin(), decisive.end(), [](const CriticVote* vote) {
            return !vote->required || vote->dominant == FindingOutcome::Pass;
        });
    };

    if (denominator == 0) {
        aggregated = ReviewDisposition::InsufficientEvidence;
        aggregation_basis = "no current decisive critic finding exists; absence of findings is not PASS";
    } else if (hard_invalid) {
        // A failed mandatory predicate is never rescued by a favorable score.
        switch (primary_rejection) {
            case ErrorCode::RevalidationRequired:
                aggregated = ReviewDisposition::RevalidationRequired;
                break;
            case ErrorCode::InsufficientEvidence:
            case ErrorCode::EvidenceIntegrityFailed:
            case ErrorCode::EvidenceProvenanceUnknown:
                aggregated = ReviewDisposition::InsufficientEvidence;
                break;
            case ErrorCode::QuorumNotReached:
                aggregated = ReviewDisposition::QuorumNotReached;
                break;
            case ErrorCode::StaleCriticGeneration:
            case ErrorCode::StaleWorkerBoot:
                aggregated = ReviewDisposition::RevalidationRequired;
                break;
            case ErrorCode::RequiredCriticFailed:
                aggregated = ReviewDisposition::RequiredCriticFailed;
                break;
            case ErrorCode::TargetDigestMismatch:
                aggregated = ReviewDisposition::RevalidationRequired;
                break;
            case ErrorCode::PolicyRejected:
                aggregated = ReviewDisposition::Unknown;
                break;
            default:
                aggregated = ReviewDisposition::Fail;
                break;
        }
        aggregation_basis = std::string("mandatory predicate failed: ") + to_string(primary_rejection);
    } else {
        std::uint32_t pass_votes = 0;
        std::uint32_t fail_votes = 0;
        std::uint32_t warn_votes = 0;
        std::uint64_t pass_weight = 0;
        std::uint64_t fail_weight = 0;
        std::uint64_t warn_weight = 0;
        for (const CriticVote* vote : decisive) {
            switch (vote->dominant) {
                case FindingOutcome::Pass:
                    pass_votes += 1;
                    pass_weight += vote->weight;
                    break;
                case FindingOutcome::Fail:
                    fail_votes += 1;
                    fail_weight += vote->weight;
                    break;
                case FindingOutcome::Warn:
                    warn_votes += 1;
                    warn_weight += vote->weight;
                    break;
                default:
                    break;
            }
        }
        const bool quorum_met = quorum.satisfied;

        switch (spec.aggregation) {
            case AggregationPolicy::AllRequiredPass: {
                if (any_critical()) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "critical fail findings exceed the policy limit";
                } else if (!quorum_met) {
                    aggregated = ReviewDisposition::QuorumNotReached;
                    aggregation_basis = quorum.basis;
                } else if (fail_weight > 0) {
                    if (all_required_fail()) {
                        aggregated = ReviewDisposition::Fail;
                        aggregation_basis = "a required critic asserted FAIL at or above the fail threshold";
                    } else if (spec.warn_counts_as_failure) {
                        aggregated = ReviewDisposition::Fail;
                        aggregation_basis = "a decisive critic asserted FAIL and the policy treats it as failure";
                    } else {
                        aggregated = ReviewDisposition::Fail;
                        aggregation_basis = "AllRequiredPass requires every required critic to pass";
                    }
                } else if (warn_weight > 0) {
                    aggregated = spec.warn_counts_as_failure ? ReviewDisposition::Fail : ReviewDisposition::Warn;
                    aggregation_basis = spec.warn_counts_as_failure
                                            ? "a decisive critic asserted WARN and the policy treats WARN as failure"
                                            : "every required critic passed but a decisive critic warned";
                } else if (all_required_pass()) {
                    aggregated = ReviewDisposition::Pass;
                    aggregation_basis = "every required critic produced a decisive PASS under quorum";
                } else {
                    aggregated = ReviewDisposition::InsufficientEvidence;
                    aggregation_basis = "no required critic produced a decisive finding";
                }
                break;
            }
            case AggregationPolicy::AnyCriticalFail: {
                if (any_critical()) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "at least one critical FAIL finding exists";
                } else if (!quorum_met) {
                    aggregated = ReviewDisposition::QuorumNotReached;
                    aggregation_basis = quorum.basis;
                } else if (fail_weight > 0) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "a decisive critic asserted FAIL";
                } else if (warn_weight > 0) {
                    aggregated = spec.warn_counts_as_failure ? ReviewDisposition::Fail : ReviewDisposition::Warn;
                    aggregation_basis = "a decisive critic asserted WARN";
                } else {
                    aggregated = ReviewDisposition::Pass;
                    aggregation_basis = "no critical FAIL and quorum satisfied";
                }
                break;
            }
            case AggregationPolicy::QuorumPass: {
                if (any_critical()) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "critical FAIL count exceeds the policy limit before quorum applies";
                } else if (pass_votes >= spec.quorum) {
                    aggregated = ReviewDisposition::Pass;
                    aggregation_basis = "quorum of PASS votes reached: " + std::to_string(pass_votes) + "/" +
                                        std::to_string(spec.quorum);
                } else if (fail_votes >= spec.quorum) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "quorum of FAIL votes reached: " + std::to_string(fail_votes) + "/" +
                                        std::to_string(spec.quorum);
                } else {
                    aggregated = ReviewDisposition::QuorumNotReached;
                    aggregation_basis = "neither PASS nor FAIL reached quorum; denominator " +
                                        std::to_string(denominator);
                }
                break;
            }
            case AggregationPolicy::WeightedReview: {
                if (any_critical()) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "critical FAIL count exceeds the policy limit; weights cannot rescue it";
                } else if (!quorum_met) {
                    aggregated = ReviewDisposition::QuorumNotReached;
                    aggregation_basis = quorum.basis;
                } else if (fail_weight > pass_weight) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "weighted FAIL exceeds weighted PASS";
                } else if (pass_weight > fail_weight + warn_weight) {
                    aggregated = ReviewDisposition::Pass;
                    aggregation_basis = "weighted PASS exceeds weighted FAIL plus WARN";
                } else if (pass_weight == fail_weight && fail_weight > 0) {
                    aggregated = ReviewDisposition::Tie;
                    aggregation_basis = "weighted PASS and weighted FAIL are equal";
                } else {
                    aggregated = ReviewDisposition::Warn;
                    aggregation_basis = "weighted WARN prevents a PASS";
                }
                break;
            }
            case AggregationPolicy::RequiredRolePass: {
                bool required_role_failed = false;
                for (const CriticVote* vote : decisive) {
                    if (spec.is_required_role(vote->role) && vote->dominant == FindingOutcome::Fail) {
                        required_role_failed = true;
                    }
                }
                if (any_critical() || required_role_failed) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "a required-role critic asserted FAIL";
                } else if (!quorum.satisfied) {
                    aggregated = ReviewDisposition::QuorumNotReached;
                    aggregation_basis = quorum.basis;
                } else {
                    bool all_required_roles_pass = true;
                    for (CriticRole role : spec.required_roles) {
                        const bool pass_present = std::any_of(
                            decisive.begin(), decisive.end(), [role](const CriticVote* vote) {
                                return vote->role == role && vote->dominant == FindingOutcome::Pass;
                            });
                        if (!pass_present) {
                            all_required_roles_pass = false;
                        }
                    }
                    if (all_required_roles_pass) {
                        aggregated = warn_weight > 0 ? ReviewDisposition::Warn : ReviewDisposition::Pass;
                        aggregation_basis = "every required role produced a decisive PASS";
                    } else {
                        aggregated = ReviewDisposition::InsufficientEvidence;
                        aggregation_basis = "a required role produced no decisive PASS";
                    }
                }
                break;
            }
            case AggregationPolicy::MajorityWithConstraints: {
                if (any_critical()) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "critical FAIL count exceeds the policy limit";
                } else if (!quorum_met) {
                    aggregated = ReviewDisposition::QuorumNotReached;
                    aggregation_basis = quorum.basis;
                } else if (pass_votes * 2 > denominator) {
                    aggregated = warn_weight > 0 ? ReviewDisposition::Warn : ReviewDisposition::Pass;
                    aggregation_basis = "strict majority among " + std::to_string(denominator) +
                                        " decisive critics passed";
                } else if (fail_votes * 2 > denominator) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "strict majority among " + std::to_string(denominator) +
                                        " decisive critics failed";
                } else {
                    aggregated = ReviewDisposition::Tie;
                    aggregation_basis = "no strict majority among " + std::to_string(denominator) +
                                        " decisive critics";
                }
                break;
            }
            case AggregationPolicy::ConsensusThreshold: {
                if (any_critical()) {
                    aggregated = ReviewDisposition::Fail;
                    aggregation_basis = "critical FAIL count exceeds the policy limit";
                } else if (denominator == 0) {
                    aggregated = ReviewDisposition::InsufficientEvidence;
                    aggregation_basis = "no decisive critics; consensus is undefined";
                } else {
                    const std::uint64_t threshold =
                        (static_cast<std::uint64_t>(denominator) * spec.consensus_threshold_basis_points) / 10000u;
                    const std::uint32_t needed =
                        static_cast<std::uint32_t>(threshold) +
                        (((static_cast<std::uint64_t>(denominator) * spec.consensus_threshold_basis_points) % 10000u != 0)
                             ? 1u
                             : 0u);
                    if (pass_votes >= needed && pass_votes > 0) {
                        aggregated = warn_weight > 0 ? ReviewDisposition::Warn : ReviewDisposition::Pass;
                        aggregation_basis = "PASS reached the consensus threshold " + std::to_string(needed) + "/" +
                                            std::to_string(denominator);
                    } else if (fail_votes >= needed && fail_votes > 0) {
                        aggregated = ReviewDisposition::Fail;
                        aggregation_basis = "FAIL reached the consensus threshold " + std::to_string(needed) + "/" +
                                            std::to_string(denominator);
                    } else {
                        aggregated = ReviewDisposition::Disagreement;
                        aggregation_basis = "no disposition reached the consensus threshold " +
                                            std::to_string(needed) + "/" + std::to_string(denominator);
                    }
                }
                break;
            }
            case AggregationPolicy::PolicyTable: {
                std::uint32_t outcome_counts[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                Severity outcome_max[8] = {Severity::Info, Severity::Info, Severity::Info, Severity::Info,
                                           Severity::Info, Severity::Info, Severity::Info, Severity::Info};
                for (const auto& entry : current) {
                    const auto index = static_cast<std::size_t>(entry.second->outcome);
                    if (index < 8) {
                        outcome_counts[index] += 1;
                        if (severity_rank(entry.second->severity) > severity_rank(outcome_max[index])) {
                            outcome_max[index] = entry.second->severity;
                        }
                    }
                }
                bool matched = false;
                for (const PolicyTableEntry& row : spec.policy_table) {
                    const auto index = static_cast<std::size_t>(row.outcome);
                    if (index >= 8 || outcome_counts[index] < row.min_count) {
                        continue;
                    }
                    const bool severity_in_range =
                        severity_rank(outcome_max[index]) >= severity_rank(row.min_severity) &&
                        severity_rank(outcome_max[index]) <= severity_rank(row.max_severity);
                    if (!severity_in_range) {
                        continue;
                    }
                    aggregated = row.disposition;
                    aggregation_basis = std::string("policy table row matched for ") + to_string(row.outcome);
                    matched = true;
                    break;
                }
                if (!matched) {
                    aggregated = ReviewDisposition::InsufficientEvidence;
                    aggregation_basis = "no policy table row matched the observed findings";
                }
                break;
            }
        }
    }

    // --- 9. Disagreement policy applies on top of aggregation ---------------
    // Genuine disagreement is never allowed to disappear behind an aggregate.
    // A decisive FAIL is an assertion rather than a tie, so it is left alone;
    // a PASS, a tie, a quorum shortfall, or an insufficiency is re-expressed
    // through the configured disagreement policy.
    const bool aggregate_is_inconclusive =
        aggregated == ReviewDisposition::QuorumNotReached || aggregated == ReviewDisposition::Tie ||
        aggregated == ReviewDisposition::InsufficientEvidence || aggregated == ReviewDisposition::Warn;
    if (disagreement.present && (aggregated == ReviewDisposition::Pass || aggregate_is_inconclusive)) {
        switch (spec.disagreement) {
            case DisagreementPolicy::ReportAndFail:
                aggregated = ReviewDisposition::Disagreement;
                disagreement.disposition = aggregated;
                aggregation_basis = "decisive critics disagreed and the policy reports disagreement as failure";
                break;
            case DisagreementPolicy::ReportAndWarn:
                aggregated = ReviewDisposition::Warn;
                disagreement.disposition = aggregated;
                aggregation_basis = "decisive critics disagreed and the policy reports disagreement as a warning";
                break;
            case DisagreementPolicy::ThresholdThenFail:
                if (disagreement.disagreeing_critic_count >= spec.disagreement_threshold_fail) {
                    aggregated = ReviewDisposition::Disagreement;
                    disagreement.disposition = aggregated;
                    aggregation_basis = "disagreeing critics reached the configured threshold";
                } else {
                    aggregation_basis = "disagreement below the configured threshold";
                }
                break;
            case DisagreementPolicy::Escalate:
                aggregated = ReviewDisposition::Disagreement;
                disagreement.disposition = aggregated;
                aggregation_basis = "the policy escalates any unresolved critic disagreement";
                break;
        }
    }

    // An unresolved bounded challenge is a live dispute about a finding. It is
    // never allowed to disappear behind a favorable aggregate.
    if (aggregated == ReviewDisposition::Pass && !ex.unresolved_challenges.empty()) {
        aggregated = ReviewDisposition::Disagreement;
        aggregation_basis = "an unresolved bounded challenge prevents a PASS disposition";
    }

    // UNKNOWN must never silently become PASS.
    if (aggregated == ReviewDisposition::Pass && ex.unknown_count > 0 && spec.unknown_counts_as_failure) {
        aggregated = ReviewDisposition::Fail;
        aggregation_basis = "the policy treats UNKNOWN findings as failure";
    }
    if (aggregated == ReviewDisposition::Pass && hard_invalid) {
        aggregated = ReviewDisposition::Fail;
        aggregation_basis = "mandatory predicate failure cannot be rescued by a favorable aggregate";
    }

    result.disposition = aggregated;
    result.hard_invalid = hard_invalid;
    result.primary_rejection = primary_rejection;
    ex.disposition = aggregated;
    ex.policy_result = spec.summarize() + " -> " + to_string(aggregated) + " (" + aggregation_basis + ")";
    ex.notes.push_back(aggregation_basis);
    ex.explanation_digest = compute_explanation_digest(ex);
    return result;
}

}  // namespace critic_fabric

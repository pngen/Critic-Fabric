// Critic Fabric — required review scenarios end to end.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace critic_fabric;
using namespace cftest;

CF_TEST(single_critic_pass_commits_with_exact_provenance) {
    Fixture fixture;
    const TargetRecord target = fixture.target("single critic pass");
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::CorrectnessVerifier};
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic critic = fixture.critic(CriticRole::CorrectnessVerifier, SyntheticBehavior::Pass);
    const auto assignments =
        fixture.assign(review.id, review.generation, {{critic, CriticRole::CorrectnessVerifier}});
    const Finding finding =
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);
    CF_CHECK(finding.outcome == FindingOutcome::Pass);
    CF_CHECK(!finding.evidence.empty());

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition == ReviewDisposition::Pass);

    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    CF_CHECK(snapshot.value().review.state == ReviewState::Committed);
    CF_CHECK(snapshot.value().decision.has_value());
    CF_CHECK(snapshot.value().findings.size() == 1);
    CF_CHECK(snapshot.value().findings.front().critic == critic.id);
    CF_CHECK(snapshot.value().findings.front().target_generation == target.generation);
    CF_CHECK(snapshot.value().findings.front().worker_boot == critic.boot);

    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK(explanation.value().contributors.size() == 1);
    CF_CHECK(!explanation.value().predicates.empty());
    for (const PredicateEvaluation& predicate : explanation.value().predicates) {
        CF_CHECK(predicate.satisfied);
    }
}

CF_TEST(single_critic_fail_is_authoritative_and_stale_pass_cannot_overwrite) {
    Fixture fixture;
    const TargetRecord target = fixture.target("single critic fail");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Fail,
                                                 FindingCategory::Correctness, Severity::Critical);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Fail, fixture.next_nonce_++);
    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition == ReviewDisposition::Fail);

    // A later PASS from the same authority against the same committed generation
    // must be rejected, because a committed generation accepts no new findings.
    const auto late = fixture.try_publish(assignments.front(), target, critic, SyntheticBehavior::Pass,
                                          fixture.next_nonce_++);
    CF_CHECK(!late.ok());
    CF_CHECK(late.code() == ErrorCode::ResultAlreadyCommitted);
}

CF_TEST(multiple_critics_agree_deterministically) {
    Fixture fixture;
    const TargetRecord target = fixture.target("multiple critics agree");
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::CorrectnessVerifier, CriticRole::SchemaVerifier};
    spec.min_critic_count = 3;
    spec.required_critic_count = 2;
    spec.quorum = 3;
    const ReviewRecord review = fixture.declare(target, spec);

    const Fixture::Critic first = fixture.critic(CriticRole::CorrectnessVerifier, SyntheticBehavior::Pass);
    const Fixture::Critic second = fixture.critic(CriticRole::SchemaVerifier, SyntheticBehavior::Pass);
    const Fixture::Critic third = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation,
                                            {{first, CriticRole::CorrectnessVerifier},
                                             {second, CriticRole::SchemaVerifier},
                                             {third, CriticRole::GeneralCritic}});
    fixture.publish(assignments[0], target, first, SyntheticBehavior::Pass, fixture.next_nonce_++);
    fixture.publish(assignments[1], target, second, SyntheticBehavior::Pass, fixture.next_nonce_++);
    fixture.publish(assignments[2], target, third, SyntheticBehavior::Pass, fixture.next_nonce_++);

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition == ReviewDisposition::Pass);

    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK_EQ(explanation.value().quorum.distinct_decisive_critics, 3u);
    CF_CHECK(explanation.value().quorum.satisfied);
    CF_CHECK(!explanation.value().disagreement.present);
    // Contributor order is by critic identity, never by unordered iteration.
    for (std::size_t i = 1; i < explanation.value().contributors.size(); ++i) {
        CF_CHECK(explanation.value().contributors[i - 1].critic <
                 explanation.value().contributors[i].critic);
    }
}

CF_TEST(multiple_critics_disagree_is_exposed_not_coerced) {
    Fixture fixture;
    const TargetRecord target = fixture.target("multiple critics disagree");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    spec.aggregation = AggregationPolicy::QuorumPass;
    spec.disagreement = DisagreementPolicy::ReportAndFail;
    // The policy tolerates one critical failure so that the question genuinely
    // becomes whether the critics agree rather than whether one failed.
    spec.max_critical_fails = 1;
    const ReviewRecord review = fixture.declare(target, spec);

    const Fixture::Critic yes = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Fixture::Critic no = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Fail);
    const auto assignments =
        fixture.assign(review.id, review.generation,
                       {{yes, CriticRole::GeneralCritic}, {no, CriticRole::GeneralCritic}});
    fixture.publish(assignments[0], target, yes, SyntheticBehavior::Pass, fixture.next_nonce_++);
    fixture.publish(assignments[1], target, no, SyntheticBehavior::Fail, fixture.next_nonce_++);

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition == ReviewDisposition::Disagreement);

    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK(explanation.value().disagreement.present);
    CF_CHECK(explanation.value().disagreement.pass_critics.size() == 1);
    CF_CHECK(explanation.value().disagreement.fail_critics.size() == 1);
    CF_CHECK(!explanation.value().disagreement.conflicting_pairs.empty());
}

CF_TEST(abstention_never_becomes_pass) {
    Fixture fixture;
    const TargetRecord target = fixture.target("abstention");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic passer = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Fixture::Critic abstainer = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Abstain);
    const auto assignments =
        fixture.assign(review.id, review.generation,
                       {{passer, CriticRole::GeneralCritic}, {abstainer, CriticRole::GeneralCritic}});
    fixture.publish(assignments[0], target, passer, SyntheticBehavior::Pass, fixture.next_nonce_++);
    fixture.publish(assignments[1], target, abstainer, SyntheticBehavior::Abstain, fixture.next_nonce_++);

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition != ReviewDisposition::Pass);
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK_EQ(explanation.value().quorum.distinct_decisive_critics, 1u);
    CF_CHECK_EQ(explanation.value().quorum.distinct_abstaining_critics, 1u);
    CF_CHECK(!explanation.value().quorum.satisfied);
}

CF_TEST(unknown_evidence_does_not_satisfy_a_mandatory_predicate) {
    Fixture fixture;
    const TargetRecord target = fixture.target("unknown outcome");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Unknown);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Unknown, fixture.next_nonce_++);

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition != ReviewDisposition::Pass);
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK_EQ(explanation.value().quorum.distinct_decisive_critics, 0u);
    CF_CHECK(explanation.value().disposition == ReviewDisposition::InsufficientEvidence);
}

CF_TEST(unknown_provenance_evidence_fails_a_hard_predicate) {
    Fixture fixture;
    const TargetRecord target = fixture.target("unknown provenance");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});

    SyntheticCriticConfig configuration = critic.config;
    configuration.provenance = EvidenceProvenance::Unknown;
    const SyntheticReviewOutput output =
        run_synthetic_critic(configuration, critic.generation, target);

    SubmitEvidenceRequest evidence_request;
    evidence_request.review = review.id;
    evidence_request.review_generation = review.generation;
    evidence_request.target = target.id;
    evidence_request.target_generation = target.generation;
    evidence_request.critic = critic.id;
    evidence_request.critic_generation = critic.generation;
    evidence_request.worker = critic.worker;
    evidence_request.worker_boot = critic.boot;
    evidence_request.source_type = output.source_type;
    evidence_request.provenance = EvidenceProvenance::Unknown;
    evidence_request.integrity = EvidenceIntegrity::Verified;
    evidence_request.payload = output.evidence_payload;
    evidence_request.epoch = fixture.epoch();
    const auto evidence = fixture.coordinator.submit_evidence(evidence_request);
    CF_REQUIRE(evidence.ok());

    SubmitFindingRequest finding_request;
    finding_request.review = review.id;
    finding_request.review_generation = review.generation;
    finding_request.target = target.id;
    finding_request.target_generation = target.generation;
    finding_request.critic = critic.id;
    finding_request.critic_generation = critic.generation;
    finding_request.worker = critic.worker;
    finding_request.worker_boot = critic.boot;
    finding_request.category = FindingCategory::Correctness;
    finding_request.severity = Severity::Critical;
    finding_request.outcome = FindingOutcome::Pass;
    finding_request.evidence = {evidence.value().id};
    finding_request.explanation = "passing finding backed by unknown-provenance evidence";
    finding_request.epoch = fixture.epoch();
    CF_REQUIRE(fixture.coordinator.submit_finding(finding_request).ok());

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition != ReviewDisposition::Pass);
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    bool found = false;
    for (const PredicateEvaluation& predicate : explanation.value().predicates) {
        if (predicate.kind == PredicateKind::EvidenceProvenanceKnown) {
            found = true;
            CF_CHECK(!predicate.satisfied);
        }
    }
    CF_CHECK(found);
}

CF_TEST(missing_evidence_blocks_pass_even_with_a_favourable_outcome) {
    Fixture fixture;
    const TargetRecord target = fixture.target("no evidence at all");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});

    SubmitFindingRequest finding_request;
    finding_request.review = review.id;
    finding_request.review_generation = review.generation;
    finding_request.target = target.id;
    finding_request.target_generation = target.generation;
    finding_request.critic = critic.id;
    finding_request.critic_generation = critic.generation;
    finding_request.worker = critic.worker;
    finding_request.worker_boot = critic.boot;
    finding_request.category = FindingCategory::Correctness;
    finding_request.severity = Severity::Critical;
    finding_request.outcome = FindingOutcome::Pass;
    finding_request.explanation = "PASS asserted with no evidence attached";
    finding_request.epoch = fixture.epoch();
    CF_REQUIRE(fixture.coordinator.submit_finding(finding_request).ok());

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition == ReviewDisposition::InsufficientEvidence);
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    for (const PredicateEvaluation& predicate : explanation.value().predicates) {
        if (predicate.kind == PredicateKind::RequiredEvidencePresent) {
            CF_CHECK(!predicate.satisfied);
        }
    }
}

CF_TEST(hard_predicate_beats_a_favourable_weighted_score) {
    Fixture fixture;
    const TargetRecord target = fixture.target("hard predicate beats score");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    spec.aggregation = AggregationPolicy::WeightedReview;
    const ReviewRecord review = fixture.declare(target, spec);

    const Fixture::Critic heavy = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Fixture::Critic light = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Fail,
                                                FindingCategory::Correctness, Severity::Critical);
    // The passing critic carries an overwhelming weight.
    ReviewSpec weighted = spec;
    weighted.critic_weights[heavy.id] = 1000;
    weighted.critic_weights[light.id] = 1;
    // The review already declared its policy, so the weighting is applied
    // directly to the assignment records the coordinator will use.
    const auto assignments =
        fixture.assign(review.id, review.generation,
                       {{heavy, CriticRole::GeneralCritic}, {light, CriticRole::GeneralCritic}});
    CF_CHECK(assignments.size() == 2);

    fixture.publish(assignments[0], target, heavy, SyntheticBehavior::Pass, fixture.next_nonce_++);
    fixture.publish(assignments[1], target, light, SyntheticBehavior::Fail, fixture.next_nonce_++);

    const Decision decision = fixture.commit(review, target.generation);
    // A mandatory predicate (no critical fail findings beyond the limit) fails,
    // so the favourable aggregate cannot rescue the review.
    CF_CHECK(decision.disposition == ReviewDisposition::Fail);
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    bool mandatory_failed = false;
    for (const PredicateEvaluation& predicate : explanation.value().predicates) {
        if (predicate.kind == PredicateKind::NoCriticalFailFindings && predicate.mandatory &&
            !predicate.satisfied) {
            mandatory_failed = true;
        }
    }
    CF_CHECK(mandatory_failed);
}

CF_TEST(target_mutation_invalidates_prior_review_authority) {
    Fixture fixture;
    TargetRecord target = fixture.target("revision one");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);

    const TargetRecord advanced = fixture.advance_target(target, "revision two");
    CF_CHECK_EQ(advanced.generation.value(), target.generation.value() + 1);

    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    CF_CHECK(snapshot.value().review.state == ReviewState::Superseded);
    CF_CHECK(!snapshot.value().review.authority_current);
    for (const Finding& finding : snapshot.value().findings) {
        CF_CHECK(finding.state == FindingState::Invalidated);
    }

    const auto late = fixture.try_publish(assignments.front(), target, critic, SyntheticBehavior::Pass,
                                          fixture.next_nonce_++);
    CF_CHECK(!late.ok());
    CF_CHECK(late.code() == ErrorCode::StaleTargetGeneration ||
             late.code() == ErrorCode::ReviewSuperseded);

    const auto commit_attempt = fixture.try_commit(review, target.generation);
    CF_CHECK(!commit_attempt.ok());

    // A fresh review against the new generation is required.
    const ReviewRecord fresh = fixture.declare(advanced, default_review_spec());
    CF_CHECK(fresh.target_generation == advanced.generation);
}

CF_TEST(committed_review_becomes_historical_but_remains_inspectable) {
    Fixture fixture;
    TargetRecord target = fixture.target("committed then superseded");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);
    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition == ReviewDisposition::Pass);

    const TargetRecord advanced = fixture.advance_target(target, "revision after commit");
    CF_CHECK(advanced.generation.value() == target.generation.value() + 1);

    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    // The historical decision survives, but it no longer describes the current
    // target generation and says so explicitly.
    CF_CHECK(snapshot.value().decision.has_value());
    CF_CHECK_EQ(snapshot.value().decision->disposition, ReviewDisposition::Pass);
    CF_CHECK(!snapshot.value().review.authority_current);
    const auto historical = fixture.coordinator.query_target_generation(target.id, target.generation);
    CF_CHECK(historical.ok());
}

CF_TEST(critic_replacement_is_generation_safe) {
    Fixture fixture;
    const TargetRecord target = fixture.target("critic replaced");
    const Fixture::Critic first = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const auto assignments = fixture.assign(review.id, review.generation, {{first, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, first, SyntheticBehavior::Pass, fixture.next_nonce_++);

    const Fixture::Critic second = fixture.advance_critic(first);
    CF_CHECK(second.generation.value() > first.generation.value());
    CF_CHECK(second.id == first.id);

    // Late traffic from the retired generation is rejected before any mutation.
    const auto late = fixture.try_publish(assignments.front(), target, first, SyntheticBehavior::Pass,
                                          fixture.next_nonce_++);
    CF_CHECK(!late.ok());

    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    CF_CHECK(snapshot.value().review.state == ReviewState::RevalidationRequired);
    for (const CriticAssignment& assignment : snapshot.value().assignments) {
        CF_CHECK(!assignment.active);
        CF_CHECK(!assignment.retirement_reason.empty());
    }
    // The old finding remains historical rather than becoming current again.
    CF_CHECK(snapshot.value().findings.size() == 1);
}

CF_TEST(duplicate_finding_is_idempotent_or_rejected_by_policy) {
    Fixture fixture;
    const TargetRecord target = fixture.target("duplicate finding");
    ReviewSpec spec = default_review_spec();
    spec.duplicates = DuplicatePolicy::Reject;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    const Finding original =
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);

    // The identical finding is replayed with the identical evidence and text, so
    // it is a genuine duplicate rather than a new distinct assessment.
    SubmitFindingRequest replay;
    replay.review = review.id;
    replay.review_generation = review.generation;
    replay.target = target.id;
    replay.target_generation = target.generation;
    replay.critic = critic.id;
    replay.critic_generation = critic.generation;
    replay.worker = critic.worker;
    replay.worker_boot = critic.boot;
    replay.category = original.category;
    replay.severity = original.severity;
    replay.outcome = original.outcome;
    replay.evidence = original.evidence;
    replay.explanation = original.explanation;
    replay.epoch = fixture.epoch();
    const auto duplicate = fixture.coordinator.submit_finding(replay);
    CF_CHECK(original.id.valid());
    CF_CHECK(!duplicate.ok());
    CF_CHECK(duplicate.code() == ErrorCode::DuplicateFinding);

    // Under IDEMPOTENT_IGNORE the identical replay returns the original finding.
    const TargetRecord other = fixture.target("idempotent duplicate policy");
    ReviewSpec lenient = default_review_spec();
    lenient.duplicates = DuplicatePolicy::IdempotentIgnore;
    const ReviewRecord lenient_review = fixture.declare(other, lenient);
    const Fixture::Critic second = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto second_assignments =
        fixture.assign(lenient_review.id, lenient_review.generation, {{second, CriticRole::GeneralCritic}});
    const Finding first =
        fixture.publish(second_assignments.front(), other, second, SyntheticBehavior::Pass, fixture.next_nonce_++);
    SubmitFindingRequest repeated;
    repeated.review = lenient_review.id;
    repeated.review_generation = lenient_review.generation;
    repeated.target = other.id;
    repeated.target_generation = other.generation;
    repeated.critic = second.id;
    repeated.critic_generation = second.generation;
    repeated.worker = second.worker;
    repeated.worker_boot = second.boot;
    repeated.category = first.category;
    repeated.severity = first.severity;
    repeated.outcome = first.outcome;
    repeated.evidence = first.evidence;
    repeated.explanation = first.explanation;
    repeated.epoch = fixture.epoch();
    const auto idempotent = fixture.coordinator.submit_finding(repeated);
    CF_REQUIRE(idempotent.ok());
    CF_CHECK(idempotent.value().id == first.id);
}

CF_TEST(conflicting_finding_from_one_critic_is_rejected) {
    Fixture fixture;
    const TargetRecord target = fixture.target("conflicting finding");
    ReviewSpec spec = default_review_spec();
    spec.duplicates = DuplicatePolicy::KeepDistinct;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);

    // Same category, same evidence set, opposite decisive outcome.
    const auto witness = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(witness.ok());
    CF_REQUIRE(!witness.value().findings.empty());
    const Finding previous = witness.value().findings.front();
    SubmitFindingRequest conflicting;
    conflicting.review = review.id;
    conflicting.review_generation = review.generation;
    conflicting.target = target.id;
    conflicting.target_generation = target.generation;
    conflicting.critic = critic.id;
    conflicting.critic_generation = critic.generation;
    conflicting.worker = critic.worker;
    conflicting.worker_boot = critic.boot;
    conflicting.category = previous.category;
    conflicting.severity = Severity::Critical;
    conflicting.outcome = FindingOutcome::Fail;
    conflicting.evidence = previous.evidence;
    conflicting.explanation = "contradictory decisive outcome for the same evidence";
    conflicting.epoch = fixture.epoch();
    const auto rejected = fixture.coordinator.submit_finding(conflicting);
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::ConflictingFinding);
}

CF_TEST(explicit_supersession_advances_the_finding_generation) {
    Fixture fixture;
    const TargetRecord target = fixture.target("explicit supersession");
    ReviewSpec spec = default_review_spec();
    spec.duplicates = DuplicatePolicy::Reject;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    const Finding first =
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);

    const auto evidence = fixture.evidence(assignments.front(), target, critic, 9001);
    SubmitFindingRequest replacement;
    replacement.review = review.id;
    replacement.review_generation = review.generation;
    replacement.target = target.id;
    replacement.target_generation = target.generation;
    replacement.critic = critic.id;
    replacement.critic_generation = critic.generation;
    replacement.worker = critic.worker;
    replacement.worker_boot = critic.boot;
    replacement.category = first.category;
    replacement.severity = Severity::Critical;
    replacement.outcome = FindingOutcome::Fail;
    replacement.evidence = {evidence.id};
    replacement.explanation = "revised assessment after re-deriving the witness";
    replacement.supersedes = first.id;
    replacement.epoch = fixture.epoch();
    const auto accepted = fixture.coordinator.submit_finding(replacement);
    CF_REQUIRE(accepted.ok());
    CF_CHECK_EQ(accepted.value().generation.value(), first.generation.value() + 1);

    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    CF_CHECK_EQ(snapshot.value().findings.size(), 2u);
    for (const Finding& finding : snapshot.value().findings) {
        if (finding.id == first.id) {
            CF_CHECK(finding.state == FindingState::Superseded);
        }
    }
}

CF_TEST(challenge_and_rebuttal_upholds_a_finding) {
    Fixture fixture;
    const TargetRecord target = fixture.target("challenge upheld");
    ReviewSpec spec = default_review_spec();
    spec.challenges = ChallengePolicy::AllowOneRound;
    spec.max_challenge_rounds = 1;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic author = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Fixture::Critic challenger = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Fail);
    const auto assignments =
        fixture.assign(review.id, review.generation,
                       {{author, CriticRole::GeneralCritic}, {challenger, CriticRole::GeneralCritic}});
    const Finding finding =
        fixture.publish(assignments[0], target, author, SyntheticBehavior::Pass, fixture.next_nonce_++);

    SubmitChallengeRequest challenge_request;
    challenge_request.challenged_finding = finding.id;
    challenge_request.challenged_finding_generation = finding.generation;
    challenge_request.disputed_predicate = "evidence.content_digest";
    challenge_request.rationale = "the witness does not cover the declared schema";
    challenge_request.review = review.id;
    challenge_request.review_generation = review.generation;
    challenge_request.target = target.id;
    challenge_request.target_generation = target.generation;
    challenge_request.critic = challenger.id;
    challenge_request.critic_generation = challenger.generation;
    challenge_request.worker = challenger.worker;
    challenge_request.worker_boot = challenger.boot;
    challenge_request.epoch = fixture.epoch();
    const auto challenge = fixture.coordinator.submit_challenge(challenge_request);
    CF_REQUIRE(challenge.ok());

    // A second challenge against the same finding exceeds the bounded rounds.
    const auto second = fixture.coordinator.submit_challenge(challenge_request);
    CF_CHECK(!second.ok());

    SubmitRebuttalRequest rebuttal_request;
    rebuttal_request.challenge = challenge.value().id;
    rebuttal_request.challenge_generation = challenge.value().generation;
    rebuttal_request.outcome = ChallengeOutcome::Upheld;
    rebuttal_request.argument = "the witness digest covers the payload and the schema digest";
    rebuttal_request.review = review.id;
    rebuttal_request.review_generation = review.generation;
    rebuttal_request.target = target.id;
    rebuttal_request.target_generation = target.generation;
    rebuttal_request.critic = author.id;
    rebuttal_request.critic_generation = author.generation;
    rebuttal_request.worker = author.worker;
    rebuttal_request.worker_boot = author.boot;
    rebuttal_request.epoch = fixture.epoch();
    const auto rebuttal = fixture.coordinator.submit_rebuttal(rebuttal_request);
    CF_REQUIRE(rebuttal.ok());

    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    for (const Finding& current : snapshot.value().findings) {
        if (current.id == finding.id) {
            CF_CHECK(current.state == FindingState::Upheld);
        }
    }
    for (const Challenge& current : snapshot.value().challenges) {
        CF_CHECK(current.resolved);
        CF_CHECK(current.outcome == ChallengeOutcome::Upheld);
    }
}

CF_TEST(unresolved_challenge_prevents_pass) {
    Fixture fixture;
    const TargetRecord target = fixture.target("unresolved challenge");
    ReviewSpec spec = default_review_spec();
    spec.challenges = ChallengePolicy::AllowOneRound;
    spec.max_challenge_rounds = 1;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic author = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Fixture::Critic challenger = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments =
        fixture.assign(review.id, review.generation,
                       {{author, CriticRole::GeneralCritic}, {challenger, CriticRole::GeneralCritic}});
    const Finding finding =
        fixture.publish(assignments[0], target, author, SyntheticBehavior::Pass, fixture.next_nonce_++);

    SubmitChallengeRequest challenge_request;
    challenge_request.challenged_finding = finding.id;
    challenge_request.challenged_finding_generation = finding.generation;
    challenge_request.disputed_predicate = "evidence.reference";
    challenge_request.rationale = "the reference is not resolvable";
    challenge_request.review = review.id;
    challenge_request.review_generation = review.generation;
    challenge_request.target = target.id;
    challenge_request.target_generation = target.generation;
    challenge_request.critic = challenger.id;
    challenge_request.critic_generation = challenger.generation;
    challenge_request.worker = challenger.worker;
    challenge_request.worker_boot = challenger.boot;
    challenge_request.epoch = fixture.epoch();
    CF_REQUIRE(fixture.coordinator.submit_challenge(challenge_request).ok());

    SubmitRebuttalRequest rebuttal_request;
    rebuttal_request.challenge = challenge_request.epoch.valid() ? ChallengeId::from_value(1) : ChallengeId{};
    rebuttal_request.challenge_generation = ChallengeGeneration::first();
    rebuttal_request.outcome = ChallengeOutcome::Unresolved;
    rebuttal_request.argument = "the disagreement stands";
    rebuttal_request.review = review.id;
    rebuttal_request.review_generation = review.generation;
    rebuttal_request.target = target.id;
    rebuttal_request.target_generation = target.generation;
    rebuttal_request.critic = author.id;
    rebuttal_request.critic_generation = author.generation;
    rebuttal_request.worker = author.worker;
    rebuttal_request.worker_boot = author.boot;
    rebuttal_request.epoch = fixture.epoch();
    CF_REQUIRE(fixture.coordinator.submit_rebuttal(rebuttal_request).ok());

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition != ReviewDisposition::Pass);
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK(!explanation.value().unresolved_challenges.empty());
}

CF_TEST(cancellation_fences_late_critic_output_and_commit) {
    Fixture fixture;
    const TargetRecord target = fixture.target("cancellation");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic first = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Fixture::Critic second = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments =
        fixture.assign(review.id, review.generation,
                       {{first, CriticRole::GeneralCritic}, {second, CriticRole::GeneralCritic}});
    fixture.publish(assignments[0], target, first, SyntheticBehavior::Pass, fixture.next_nonce_++);

    CancelReviewRequest cancel;
    cancel.review = review.id;
    cancel.review_generation = review.generation;
    cancel.reason = "operator cancelled";
    cancel.epoch = fixture.epoch();
    const auto cancelled = fixture.coordinator.cancel_review(cancel);
    CF_REQUIRE(cancelled.ok());
    CF_CHECK(cancelled.value().state == ReviewState::Cancelled);

    // Cancellation is idempotent.
    const auto again = fixture.coordinator.cancel_review(cancel);
    CF_CHECK(again.ok());

    const auto late = fixture.try_publish(assignments[1], target, second, SyntheticBehavior::Pass,
                                          fixture.next_nonce_++);
    CF_CHECK(!late.ok());
    CF_CHECK(late.code() == ErrorCode::ReviewCancelled);

    const auto commit_attempt = fixture.try_commit(review, target.generation);
    CF_CHECK(!commit_attempt.ok());
    CF_CHECK(commit_attempt.code() == ErrorCode::ReviewCancelled);

    // A cancelled review cannot be cancelled into a success, and a committed
    // review can never be cancelled afterwards.
    const TargetRecord other = fixture.target("committed then cancel attempted");
    const ReviewRecord committed_review = fixture.declare(other, default_review_spec());
    const Fixture::Critic third = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto third_assignments =
        fixture.assign(committed_review.id, committed_review.generation, {{third, CriticRole::GeneralCritic}});
    fixture.publish(third_assignments.front(), other, third, SyntheticBehavior::Pass, fixture.next_nonce_++);
    CF_REQUIRE(fixture.try_commit(committed_review, other.generation).ok());
    CancelReviewRequest late_cancel;
    late_cancel.review = committed_review.id;
    late_cancel.review_generation = committed_review.generation;
    late_cancel.reason = "too late";
    late_cancel.epoch = fixture.epoch();
    const auto refused = fixture.coordinator.cancel_review(late_cancel);
    CF_CHECK(!refused.ok());
    CF_CHECK(refused.code() == ErrorCode::ReviewClosed);
}

CF_TEST(exactly_one_authoritative_commit_per_review_generation) {
    Fixture fixture;
    const TargetRecord target = fixture.target("single commit per generation");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);

    const Decision first = fixture.commit(review, target.generation);
    const Decision second = fixture.commit(review, target.generation);
    CF_CHECK(first.id == second.id);
    CF_CHECK(first.decision_digest == second.decision_digest);

    const auto repeated = fixture.try_commit(review, target.generation);
    CF_CHECK(repeated.ok());
    CF_CHECK(repeated.value().id == first.id);

    // A new finding after commit is impossible, so a conflicting commit cannot
    // be manufactured through critic output; it is refused structurally.
    const auto late = fixture.try_publish(assignments.front(), target, critic, SyntheticBehavior::Fail,
                                          fixture.next_nonce_++);
    CF_CHECK(!late.ok());
}

CF_TEST(review_request_identity_is_idempotent) {
    Fixture fixture;
    const TargetRecord target = fixture.target("idempotent declaration");
    ReviewSpec spec = default_review_spec();
    DeclareReviewRequest request;
    request.request = ReviewRequestId::from_value(4242);
    request.target = target.id;
    request.target_generation = target.generation;
    request.spec = spec;
    request.epoch = fixture.epoch();
    const auto first = fixture.coordinator.declare_review(request);
    CF_REQUIRE(first.ok());
    const auto second = fixture.coordinator.declare_review(request);
    CF_REQUIRE(second.ok());
    CF_CHECK(first.value().id == second.value().id);

    // The same request identity against a different target is a conflict.
    const TargetRecord other = fixture.target("different target");
    request.target = other.id;
    request.target_generation = other.generation;
    const auto conflict = fixture.coordinator.declare_review(request);
    CF_CHECK(!conflict.ok());
    CF_CHECK(conflict.code() == ErrorCode::ReviewAlreadyExists);
}

CF_TEST(quorum_cannot_be_inflated_by_a_second_worker_for_one_critic) {
    Fixture fixture;
    const TargetRecord target = fixture.target("duplicate critic identity");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});

    // A second assignment for the same logical critic is refused outright, and
    // that refusal is reported before any worker eligibility question.
    const auto duplicate = fixture.try_assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    CF_CHECK(!duplicate.ok());
    CF_CHECK(duplicate.code() == ErrorCode::DuplicateCriticIdentity);

    // A second process claims the same logical critic identity.
    const WorkerBootId second_boot = generate_worker_boot_id("test-worker-b");
    RegisterWorkerRequest worker_request;
    worker_request.boot = second_boot;
    worker_request.critic = critic.id;
    worker_request.critic_generation = critic.generation;
    worker_request.process_label = "test-worker-b";
    worker_request.epoch = fixture.epoch();
    const auto second_worker = fixture.coordinator.register_worker(worker_request);
    CF_REQUIRE(second_worker.ok());

    // Claiming the identity from a second incarnation fences the first, and the
    // review is moved to revalidation rather than silently continuing with an
    // unclear authority chain.
    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    CF_CHECK(snapshot.value().review.state == ReviewState::RevalidationRequired);
    for (const CriticAssignment& assignment : snapshot.value().assignments) {
        CF_CHECK(!assignment.active);
    }
    const auto refused = fixture.try_commit(review, target.generation);
    CF_CHECK(!refused.ok());
    CF_CHECK(refused.code() == ErrorCode::RevalidationRequired);

    // A distinct logical critic, by contrast, contributes one vote of its own.
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK(explanation.value().quorum.distinct_decisive_critics <= 1u);
}

CF_TEST(required_role_must_be_covered_before_pass) {
    Fixture fixture;
    const TargetRecord target = fixture.target("required role missing");
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::SecurityCritic};
    spec.optional_roles = {CriticRole::GeneralCritic};
    spec.min_critic_count = 1;
    spec.quorum = 1;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic general = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{general, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, general, SyntheticBehavior::Pass, fixture.next_nonce_++);

    const Decision decision = fixture.commit(review, target.generation);
    CF_CHECK(decision.disposition != ReviewDisposition::Pass);
    const auto explanation = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK(!explanation.value().quorum.uncovered_required_roles.empty());
}

CF_TEST(identical_authoritative_inputs_produce_identical_decisions) {
    // Two independent coordinators are given bit-identical authoritative inputs:
    // the same target content, the same pinned critic and worker identities, and
    // the same policy. Their committed decision digests must agree, otherwise the
    // digest would be useless to a downstream system comparing outcomes.
    const auto run_scenario = [](std::uint64_t critic_seed) {
        Fixture fixture;
        const TargetRecord target = fixture.target("deterministic decision target");
        ReviewSpec spec = default_review_spec();
        spec.required_roles = {CriticRole::CorrectnessVerifier};
        spec.min_critic_count = 2;
        spec.quorum = 2;
        const ReviewRecord review = fixture.declare(target, spec);

        // Register the critics under explicit identities and pinned boot ids.
        std::vector<Fixture::Critic> critics;
        for (int index = 0; index < 2; ++index) {
            SyntheticCriticConfig synthetic;
            synthetic.role = CriticRole::CorrectnessVerifier;
            synthetic.behavior = SyntheticBehavior::Pass;

            RegisterCriticRequest critic_request;
            critic_request.id = CriticId::from_value(critic_seed + static_cast<std::uint64_t>(index));
            critic_request.capability = synthetic_critic_capability(synthetic);
            critic_request.epoch = fixture.epoch();
            const auto registration = fixture.coordinator.register_critic(critic_request);
            CF_REQUIRE(registration.ok());

            RegisterWorkerRequest worker_request;
            worker_request.id = WorkerId::from_value(critic_seed + static_cast<std::uint64_t>(index));
            worker_request.boot = WorkerBootId::from_value(critic_seed + 100u + static_cast<std::uint64_t>(index));
            worker_request.critic = registration.value().id;
            worker_request.critic_generation = registration.value().generation;
            worker_request.epoch = fixture.epoch();
            const auto worker = fixture.coordinator.register_worker(worker_request);
            CF_REQUIRE(worker.ok());

            DeclareReadinessRequest readiness;
            readiness.id = worker.value().id;
            readiness.boot = worker_request.boot;
            readiness.critic = registration.value().id;
            readiness.critic_generation = registration.value().generation;
            readiness.ready = true;
            readiness.healthy = true;
            readiness.capability_evidence = synthetic_capability_evidence(synthetic);
            readiness.epoch = fixture.epoch();
            CF_REQUIRE(fixture.coordinator.declare_readiness(readiness).ok());

            Fixture::Critic critic;
            critic.id = registration.value().id;
            critic.generation = registration.value().generation;
            critic.worker = worker.value().id;
            critic.boot = worker_request.boot;
            synthetic.critic = critic.id;
            critic.config = synthetic;
            critics.push_back(critic);
        }

        const auto assignments = fixture.assign(review.id, review.generation,
                                                {{critics[0], CriticRole::CorrectnessVerifier},
                                                 {critics[1], CriticRole::CorrectnessVerifier}});
        for (std::size_t index = 0; index < critics.size(); ++index) {
            Fixture::Critic pinned = critics[index];
            SyntheticCriticConfig configuration = pinned.config;
            configuration.behavior = SyntheticBehavior::Pass;
            const SyntheticReviewOutput output =
                run_synthetic_critic(configuration, pinned.generation, target);

            SubmitEvidenceRequest evidence_request;
            evidence_request.review = review.id;
            evidence_request.review_generation = review.generation;
            evidence_request.target = target.id;
            evidence_request.target_generation = target.generation;
            evidence_request.critic = pinned.id;
            evidence_request.critic_generation = pinned.generation;
            evidence_request.worker = pinned.worker;
            evidence_request.worker_boot = pinned.boot;
            evidence_request.source_type = output.source_type;
            evidence_request.provenance = output.provenance;
            evidence_request.integrity = EvidenceIntegrity::Verified;
            evidence_request.reference = "determinism-witness";
            evidence_request.payload = output.evidence_payload;
            evidence_request.epoch = fixture.epoch();
            evidence_request.request = RequestId::from_value(7000 + index);
            const auto evidence = fixture.coordinator.submit_evidence(evidence_request);
            CF_REQUIRE(evidence.ok());

            SubmitFindingRequest finding_request;
            finding_request.review = review.id;
            finding_request.review_generation = review.generation;
            finding_request.target = target.id;
            finding_request.target_generation = target.generation;
            finding_request.critic = pinned.id;
            finding_request.critic_generation = pinned.generation;
            finding_request.worker = pinned.worker;
            finding_request.worker_boot = pinned.boot;
            finding_request.category = output.category;
            finding_request.severity = output.severity;
            finding_request.outcome = output.outcome;
            finding_request.evidence = {evidence.value().id};
            finding_request.explanation = "determinism assessment";
            finding_request.epoch = fixture.epoch();
            finding_request.request = RequestId::from_value(7100 + index);
            CF_REQUIRE(fixture.coordinator.submit_finding(finding_request).ok());
        }

        const Decision decision = fixture.commit(review, target.generation);
        const auto explanation = fixture.coordinator.explain_review(review.id);
        CF_REQUIRE(explanation.ok());
        struct Outcome {
            Digest decision;
            Digest explanation;
            Digest state;
            ReviewDisposition disposition{};
        };
        return Outcome{decision.decision_digest, explanation.value().explanation_digest,
                       fixture.coordinator.authoritative_state_digest(), decision.disposition};
    };

    const auto first = run_scenario(5000);
    const auto second = run_scenario(5000);
    CF_CHECK(first.disposition == ReviewDisposition::Pass);
    CF_CHECK(second.disposition == ReviewDisposition::Pass);
    // Identical authoritative inputs, including pinned incarnations, must yield
    // identical decision, explanation, and state digests.
    CF_CHECK(first.decision == second.decision);
    CF_CHECK(first.explanation == second.explanation);
    CF_CHECK(first.state == second.state);

    // A different set of critic identities is a genuinely different authority
    // basis: the same disposition follows from the same evidence, but the
    // decision digest names different authorities and therefore differs. The
    // digest is sensitive to which critics decided, and insensitive to the
    // incidental process incarnation that ran them.
    const auto third = run_scenario(9000);
    CF_CHECK(third.disposition == ReviewDisposition::Pass);
    CF_CHECK(third.decision != first.decision);
    CF_CHECK(third.explanation != first.explanation);

    // Re-running the very first scenario again must reproduce it exactly, which
    // rules out any dependence on allocation order or elapsed time.
    const auto repeat = run_scenario(5000);
    CF_CHECK(repeat.decision == first.decision);
    CF_CHECK(repeat.explanation == first.explanation);
    CF_CHECK(repeat.state == first.state);
}

CF_TEST(explanations_are_deterministic_for_identical_inputs) {
    Fixture fixture;
    const TargetRecord target = fixture.target("deterministic explanation");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);

    const auto first = fixture.coordinator.explain_review(review.id);
    const auto second = fixture.coordinator.explain_review(review.id);
    CF_REQUIRE(first.ok());
    CF_REQUIRE(second.ok());
    CF_CHECK(first.value().explanation_digest == second.value().explanation_digest);
    CF_CHECK(first.value().render() == second.value().render());
    CF_CHECK(!first.value().render().empty());
}

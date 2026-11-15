// Critic Fabric — regression tests for audited correctness defects.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

#include "fixture.hpp"

using namespace critic_fabric;
using namespace cftest;

namespace {

DeclareReadinessRequest readiness_for(const Fixture& fixture, const Fixture::Critic& critic) {
    DeclareReadinessRequest readiness;
    readiness.id = critic.worker;
    readiness.boot = critic.boot;
    readiness.critic = critic.id;
    readiness.critic_generation = critic.generation;
    readiness.ready = true;
    readiness.healthy = true;
    readiness.capability_evidence = synthetic_capability_evidence(critic.config);
    readiness.epoch = fixture.epoch();
    return readiness;
}

// A fresh process for the same logical critic: new worker identity, new boot.
Fixture::Critic reincarnate(Fixture& fixture, const Fixture::Critic& previous) {
    Fixture::Critic result = previous;
    result.boot = generate_worker_boot_id("test-worker-reincarnation");
    RegisterWorkerRequest request;
    request.boot = result.boot;
    request.critic = previous.id;
    request.critic_generation = previous.generation;
    request.process_label = "test-worker-reincarnation";
    request.epoch = fixture.epoch();
    const auto worker = fixture.coordinator.register_worker(request);
    CF_REQUIRE(worker.ok());
    result.worker = worker.value().id;
    CF_REQUIRE(fixture.coordinator.declare_readiness(readiness_for(fixture, result)).ok());
    return result;
}

}  // namespace

CF_TEST(a_fenced_incarnation_cannot_declare_readiness_again) {
    Fixture fixture;
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    CF_REQUIRE(fixture.coordinator.fence_worker(critic.worker, "operator observed a hung process").ok());
    // The retired boot identity asks for authority again.
    const auto readiness = fixture.coordinator.declare_readiness(readiness_for(fixture, critic));
    CF_CHECK(!readiness.ok());
    const auto worker = fixture.coordinator.query_worker(critic.worker);
    CF_REQUIRE(worker.ok());
    CF_CHECK(!worker.value().authoritative);
}

CF_TEST(a_superseded_incarnation_does_not_block_reassigning_its_critic) {
    Fixture fixture;
    const TargetRecord target = fixture.target("reincarnated critic");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    (void)fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);
    // The process that published exits, and a fresh incarnation takes over the
    // logical critic: the old finding no longer belongs to the authority chain.
    CF_REQUIRE(fixture.coordinator.fence_worker(critic.worker, "worker connection was lost").ok());
    const Fixture::Critic successor = reincarnate(fixture, critic);
    const auto reassigned =
        fixture.try_assign(review.id, review.generation, {{successor, CriticRole::GeneralCritic}});
    CF_CHECK_MSG(reassigned.ok(), reassigned.ok() ? "" : to_string(reassigned.code()));
}

CF_TEST(a_finding_can_be_superseded_with_a_revised_outcome_on_the_same_evidence) {
    Fixture fixture;
    const TargetRecord target = fixture.target("revised finding");
    ReviewSpec spec = default_review_spec();
    spec.duplicates = DuplicatePolicy::Reject;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    const Finding first =
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);
    CF_REQUIRE(first.outcome == FindingOutcome::Pass);

    SubmitFindingRequest revised;
    revised.review = review.id;
    revised.review_generation = review.generation;
    revised.target = target.id;
    revised.target_generation = target.generation;
    revised.critic = critic.id;
    revised.critic_generation = critic.generation;
    revised.worker = critic.worker;
    revised.worker_boot = critic.boot;
    revised.category = first.category;
    revised.severity = first.severity;
    revised.outcome = FindingOutcome::Fail;
    revised.evidence = first.evidence;  // the same evidence, re-assessed
    revised.explanation = "the witness does not hold after all";
    revised.supersedes = first.id;
    revised.epoch = fixture.epoch();
    const auto accepted = fixture.coordinator.submit_finding(revised);
    CF_CHECK_MSG(accepted.ok(), accepted.ok() ? "" : to_string(accepted.code()));
    CF_REQUIRE(accepted.ok());
    CF_CHECK(accepted.value().outcome == FindingOutcome::Fail);
    CF_CHECK_EQ(accepted.value().generation.value(), first.generation.value() + 1);
}

CF_TEST(an_allocated_target_identity_never_collides_with_an_explicit_one) {
    Fixture fixture;
    RegisterTargetRequest explicit_request;
    explicit_request.id = TargetId::from_value(2);
    explicit_request.schema_name = "test/target";
    const std::string text = "explicitly identified target";
    explicit_request.payload.assign(text.begin(), text.end());
    explicit_request.provenance = "test-fixture";
    explicit_request.epoch = fixture.epoch();
    const auto explicit_target = fixture.coordinator.register_target(explicit_request);
    CF_REQUIRE(explicit_target.ok());
    const ReviewRecord review = fixture.declare(explicit_target.value(), default_review_spec());

    // Two targets registered without an identity: neither may land on 2.
    const TargetRecord first = fixture.target("first allocated target");
    const TargetRecord second = fixture.target("second allocated target");
    CF_CHECK(first.id != explicit_target.value().id);
    CF_CHECK(second.id != explicit_target.value().id);
    const auto current = fixture.coordinator.query_target(explicit_target.value().id);
    CF_REQUIRE(current.ok());
    CF_CHECK(current.value().generation == explicit_target.value().generation);
    CF_CHECK(current.value().content_digest == explicit_target.value().content_digest);
    const auto snapshot = fixture.coordinator.query_review(review.id);
    CF_REQUIRE(snapshot.ok());
    CF_CHECK(snapshot.value().review.state != ReviewState::Superseded);
}

CF_TEST(an_allocated_worker_identity_never_takes_over_an_explicit_one) {
    Fixture fixture;
    const Fixture::Critic owner = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    // A second process for another critic registers under an explicit worker
    // identity just above the one allocated so far.
    const Fixture::Critic other = fixture.critic(CriticRole::LogicCritic, SyntheticBehavior::Pass,
                                                 FindingCategory::Correctness, Severity::Critical, false);
    RegisterWorkerRequest pinned;
    pinned.id = WorkerId::from_value(owner.worker.value() + 2);
    pinned.boot = generate_worker_boot_id("pinned-worker");
    pinned.critic = other.id;
    pinned.critic_generation = other.generation;
    pinned.process_label = "pinned-worker";
    pinned.epoch = fixture.epoch();
    const auto pinned_worker = fixture.coordinator.register_worker(pinned);
    CF_REQUIRE(pinned_worker.ok());

    // Further workers registered without an identity must not reuse it.
    const Fixture::Critic third = fixture.critic(CriticRole::SafetyCritic, SyntheticBehavior::Pass);
    const Fixture::Critic fourth = fixture.critic(CriticRole::FactChecker, SyntheticBehavior::Pass);
    CF_CHECK(third.worker != pinned.id);
    CF_CHECK(fourth.worker != pinned.id);
    const auto kept = fixture.coordinator.query_worker(pinned.id);
    CF_REQUIRE(kept.ok());
    CF_CHECK(kept.value().critic == other.id);
    CF_CHECK(kept.value().boot == pinned.boot);
}

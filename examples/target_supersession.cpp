// Critic Fabric example — target mutation invalidates prior review authority.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("target supersession");

    Harness harness(false, "");
    TargetRecord target = harness.make_target("revision one");
    const ReviewRecord review = harness.declare(target, default_review_spec());
    const Harness::Critic critic = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments =
        harness.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    harness.publish(assignments.front(), target, critic, SyntheticBehavior::Pass);

    RegisterTargetRequest revision;
    revision.id = target.id;
    revision.target_class = target.target_class;
    revision.schema_name = target.schema_name;
    const std::string payload = "revision two";
    revision.payload.assign(payload.begin(), payload.end());
    revision.provenance = "example-harness";
    revision.epoch = harness.epoch();
    const auto advanced = harness.coordinator.register_target(revision);
    Harness::require(advanced.ok(), advanced.status());
    std::printf("target generation advanced %llu -> %llu\n",
                static_cast<unsigned long long>(target.generation.value()),
                static_cast<unsigned long long>(advanced.value().generation.value()));

    const auto snapshot = harness.coordinator.query_review(review.id);
    Harness::require(snapshot.ok(), snapshot.status());
    std::printf("review state is now %s, authority_current=%d\n", to_string(snapshot.value().review.state),
                snapshot.value().review.authority_current ? 1 : 0);

    SubmitFindingRequest late;
    late.review = review.id;
    late.review_generation = review.generation;
    late.target = target.id;
    late.target_generation = target.generation;
    late.critic = critic.id;
    late.critic_generation = critic.generation;
    late.worker = critic.worker;
    late.worker_boot = critic.boot;
    late.outcome = FindingOutcome::Pass;
    late.explanation = "late finding against a superseded target generation";
    late.epoch = harness.epoch();
    const auto rejected = harness.coordinator.submit_finding(late);
    Harness::require(!rejected.ok(), "a stale finding must be rejected");
    std::printf("late finding rejected with %s\n", to_string(rejected.code()));

    const auto commit_attempt = harness.coordinator.commit_review([&] {
        CommitReviewRequest request;
        request.review = review.id;
        request.review_generation = review.generation;
        request.target_generation = target.generation;
        request.epoch = harness.epoch();
        return request;
    }());
    Harness::require(!commit_attempt.ok(), "a superseded review must not commit");
    std::printf("commit rejected with %s\n", to_string(commit_attempt.code()));
    // Either fence is a correct outcome: the review itself was superseded, or
    // the target generation binding no longer matches. Both are typed, and
    // neither is a silent success.
    const bool fenced = rejected.code() == ErrorCode::StaleTargetGeneration ||
                        rejected.code() == ErrorCode::ReviewSuperseded ||
                        rejected.code() == ErrorCode::TargetSuperseded;
    return fenced ? 0 : 1;
}

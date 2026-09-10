// Critic Fabric example — real cancellation fences late critic output.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("cancellation");

    Harness harness(false, "");
    const TargetRecord target = harness.make_target("review cancelled while a critic is active");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    const ReviewRecord review = harness.declare(target, spec);
    const Harness::Critic first = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Harness::Critic second = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments =
        harness.assign(review.id, review.generation, {{first, CriticRole::GeneralCritic},
                                                      {second, CriticRole::GeneralCritic}});
    harness.publish(assignments[0], target, first, SyntheticBehavior::Pass);

    CancelReviewRequest cancel;
    cancel.review = review.id;
    cancel.review_generation = review.generation;
    cancel.reason = "superseded by an operator decision";
    cancel.epoch = harness.epoch();
    const auto cancelled = harness.coordinator.cancel_review(cancel);
    Harness::require(cancelled.ok(), cancelled.status());
    std::printf("review state %s\n", to_string(cancelled.value().state));

    const auto late = harness.coordinator.submit_finding([&] {
        SubmitFindingRequest request;
        request.review = review.id;
        request.review_generation = review.generation;
        request.target = target.id;
        request.target_generation = target.generation;
        request.critic = second.id;
        request.critic_generation = second.generation;
        request.worker = second.worker;
        request.worker_boot = second.boot;
        request.outcome = FindingOutcome::Pass;
        request.epoch = harness.epoch();
        return request;
    }());
    Harness::require(!late.ok(), "late findings after cancellation must be rejected");
    std::printf("late finding rejected with %s\n", to_string(late.code()));

    const auto commit_attempt = harness.coordinator.commit_review([&] {
        CommitReviewRequest request;
        request.review = review.id;
        request.review_generation = review.generation;
        request.target_generation = target.generation;
        request.epoch = harness.epoch();
        return request;
    }());
    Harness::require(!commit_attempt.ok(), "a cancelled review must not commit a success");
    std::printf("commit rejected with %s\n", to_string(commit_attempt.code()));
    return late.code() == ErrorCode::ReviewCancelled ? 0 : 1;
}

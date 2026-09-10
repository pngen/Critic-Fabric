// Critic Fabric example — bounded challenge and rebuttal.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("challenge and rebuttal");

    Harness harness(false, "");
    const TargetRecord target = harness.make_target("a finding is challenged and upheld");
    ReviewSpec spec = default_review_spec();
    spec.challenges = ChallengePolicy::AllowOneRound;
    spec.max_challenge_rounds = 1;
    const ReviewRecord review = harness.declare(target, spec);

    const Harness::Critic author = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Harness::Critic challenger = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Fail);
    const auto assignments =
        harness.assign(review.id, review.generation, {{author, CriticRole::GeneralCritic},
                                                      {challenger, CriticRole::GeneralCritic}});
    const Finding finding = harness.publish(assignments[0], target, author, SyntheticBehavior::Pass);

    SubmitChallengeRequest challenge_request;
    challenge_request.challenged_finding = finding.id;
    challenge_request.challenged_finding_generation = finding.generation;
    challenge_request.disputed_predicate = "evidence.content_digest";
    challenge_request.rationale = "the witness digest does not cover the schema name";
    challenge_request.review = review.id;
    challenge_request.review_generation = review.generation;
    challenge_request.target = target.id;
    challenge_request.target_generation = target.generation;
    challenge_request.critic = challenger.id;
    challenge_request.critic_generation = challenger.generation;
    challenge_request.worker = challenger.worker;
    challenge_request.worker_boot = challenger.boot;
    challenge_request.epoch = harness.epoch();
    const auto challenge = harness.coordinator.submit_challenge(challenge_request);
    Harness::require(challenge.ok(), challenge.status());
    std::printf("challenge #%llu round %llu raised\n",
                static_cast<unsigned long long>(challenge.value().id.value()),
                static_cast<unsigned long long>(challenge.value().generation.value()));

    SubmitRebuttalRequest rebuttal_request;
    rebuttal_request.challenge = challenge.value().id;
    rebuttal_request.challenge_generation = challenge.value().generation;
    rebuttal_request.outcome = ChallengeOutcome::Upheld;
    rebuttal_request.argument = "the witness digest covers the payload and the declared schema digest";
    rebuttal_request.review = review.id;
    rebuttal_request.review_generation = review.generation;
    rebuttal_request.target = target.id;
    rebuttal_request.target_generation = target.generation;
    rebuttal_request.critic = author.id;
    rebuttal_request.critic_generation = author.generation;
    rebuttal_request.worker = author.worker;
    rebuttal_request.worker_boot = author.boot;
    rebuttal_request.epoch = harness.epoch();
    const auto rebuttal = harness.coordinator.submit_rebuttal(rebuttal_request);
    Harness::require(rebuttal.ok(), rebuttal.status());
    std::printf("rebuttal #%llu resolved as %s\n",
                static_cast<unsigned long long>(rebuttal.value().id.value()),
                to_string(challenge.value().outcome));

    const auto snapshot = harness.coordinator.query_review(review.id);
    Harness::require(snapshot.ok(), snapshot.status());
    for (const Finding& current : snapshot.value().findings) {
        std::printf("finding #%llu is now %s\n",
                    static_cast<unsigned long long>(current.id.value()), to_string(current.state));
    }
    return 0;
}

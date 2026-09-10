// Critic Fabric example — abstention never becomes PASS and quorum is explicit.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("required critic and abstention");

    Harness harness(false, "");
    const TargetRecord target = harness.make_target("one required verifier abstains");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    const ReviewRecord review = harness.declare(target, spec);

    const Harness::Critic verifier = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Harness::Critic abstainer = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Abstain);
    const auto assignments =
        harness.assign(review.id, review.generation, {{verifier, CriticRole::GeneralCritic},
                                                      {abstainer, CriticRole::GeneralCritic}});
    harness.publish(assignments[0], target, verifier, SyntheticBehavior::Pass);
    harness.publish(assignments[1], target, abstainer, SyntheticBehavior::Abstain);

    const Decision decision = harness.commit(review);
    show_decision(decision, "committed");
    std::printf("an abstention never counts towards a PASS; disposition is %s\n",
                to_string(decision.disposition));
    return decision.disposition != ReviewDisposition::Pass ? 0 : 1;
}

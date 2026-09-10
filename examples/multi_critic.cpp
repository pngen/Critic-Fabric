// Critic Fabric example — several independent critics agree deterministically.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("multiple critics agree");

    Harness harness(false, "");
    const TargetRecord target = harness.make_target("three independent verifications agree");
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::CorrectnessVerifier, CriticRole::SchemaVerifier};
    spec.optional_roles = {CriticRole::GeneralCritic};
    spec.min_critic_count = 3;
    spec.required_critic_count = 2;
    spec.quorum = 3;
    const ReviewRecord review = harness.declare(target, spec);

    const Harness::Critic first = harness.make_critic(CriticRole::CorrectnessVerifier, SyntheticBehavior::Pass);
    const Harness::Critic second = harness.make_critic(CriticRole::SchemaVerifier, SyntheticBehavior::Pass);
    const Harness::Critic third = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = harness.assign(review.id, review.generation,
                                            {{first, CriticRole::CorrectnessVerifier},
                                             {second, CriticRole::SchemaVerifier},
                                             {third, CriticRole::GeneralCritic}});
    for (std::size_t index = 0; index < assignments.size(); ++index) {
        const Harness::Critic& critic = index == 0 ? first : (index == 1 ? second : third);
        harness.publish(assignments[index], target, critic, SyntheticBehavior::Pass);
    }

    const Decision decision = harness.commit(review);
    show_decision(decision, "committed");
    const auto explanation = harness.coordinator.explain_review(review.id);
    Harness::require(explanation.ok(), explanation.status());
    std::printf("contributors: %zu, quorum satisfied: %d\n", explanation.value().contributors.size(),
                explanation.value().quorum.satisfied ? 1 : 0);
    return decision.disposition == ReviewDisposition::Pass ? 0 : 1;
}

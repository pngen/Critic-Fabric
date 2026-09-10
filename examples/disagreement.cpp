// Critic Fabric example — critic disagreement is first class and never coerced.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("critics disagree");

    Harness harness(false, "");
    const TargetRecord target = harness.make_target("one verifier passes, another asserts failure");
    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = 2;
    spec.quorum = 2;
    spec.aggregation = AggregationPolicy::QuorumPass;
    spec.disagreement = DisagreementPolicy::ReportAndFail;
    // The policy tolerates one critical failure so the question genuinely
    // becomes whether the critics agree, rather than whether one of them failed.
    spec.max_critical_fails = 1;
    const ReviewRecord review = harness.declare(target, spec);

    const Harness::Critic yes = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const Harness::Critic no = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Fail);
    const auto assignments =
        harness.assign(review.id, review.generation, {{yes, CriticRole::GeneralCritic},
                                                      {no, CriticRole::GeneralCritic}});
    harness.publish(assignments[0], target, yes, SyntheticBehavior::Pass);
    harness.publish(assignments[1], target, no, SyntheticBehavior::Fail);

    const Decision decision = harness.commit(review);
    show_decision(decision, "committed");
    const auto explanation = harness.coordinator.explain_review(review.id);
    Harness::require(explanation.ok(), explanation.status());
    std::printf("disagreement present: %d, conflicting pairs: %zu\n",
                explanation.value().disagreement.present ? 1 : 0,
                explanation.value().disagreement.conflicting_pairs.size());
    for (const std::string& pair : explanation.value().disagreement.conflicting_pairs) {
        std::printf("  pair: %s\n", pair.c_str());
    }
    return decision.disposition == ReviewDisposition::Disagreement ? 0 : 1;
}

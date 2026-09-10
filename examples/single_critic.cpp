// Critic Fabric example — one authoritative critic, one committed PASS.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("single critic PASS");

    Harness harness(false, "");
    const TargetRecord target = harness.make_target("the artifact compiles and links");
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::CorrectnessVerifier};
    const ReviewRecord review = harness.declare(target, spec);

    const Harness::Critic critic = harness.make_critic(CriticRole::CorrectnessVerifier, SyntheticBehavior::Pass);
    const auto assignments = harness.assign(review.id, review.generation,
                                            {{critic, CriticRole::CorrectnessVerifier}});
    const Finding finding = harness.publish(assignments.front(), target, critic, SyntheticBehavior::Pass);

    std::printf("finding #%llu outcome=%s severity=%s evidence=%zu\n",
                static_cast<unsigned long long>(finding.id.value()), to_string(finding.outcome),
                to_string(finding.severity), finding.evidence.size());

    const Decision decision = harness.commit(review);
    show_decision(decision, "committed");

    const auto explanation = harness.coordinator.explain_review(review.id);
    Harness::require(explanation.ok(), explanation.status());
    std::printf("quorum: %s\n", explanation.value().quorum.basis.c_str());
    std::printf("explanation digest: %s\n",
                explanation.value().explanation_digest.to_hex().substr(0, 16).c_str());
    return decision.disposition == ReviewDisposition::Pass ? 0 : 1;
}

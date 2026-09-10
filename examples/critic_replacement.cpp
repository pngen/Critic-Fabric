// Critic Fabric example — critic replacement is generation safe.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "example_support.hpp"

int main() {
    using namespace critic_fabric;
    using namespace example;
    banner("critic replacement");

    Harness harness(false, "");
    const TargetRecord target = harness.make_target("critic implementation replaced mid review");

    SyntheticCriticConfig first_config;
    first_config.role = CriticRole::GeneralCritic;
    first_config.behavior = SyntheticBehavior::Pass;

    RegisterCriticRequest registration;
    registration.capability = synthetic_critic_capability(first_config);
    registration.epoch = harness.epoch();
    const auto first = harness.coordinator.register_critic(registration);
    Harness::require(first.ok(), first.status());
    std::printf("critic %llu registered at generation %llu\n",
                static_cast<unsigned long long>(first.value().id.value()),
                static_cast<unsigned long long>(first.value().generation.value()));

    SyntheticCriticConfig second_config = first_config;
    second_config.implementation_version = "2.0.0";
    RegisterCriticRequest replacement;
    replacement.id = first.value().id;
    replacement.capability = synthetic_critic_capability(second_config);
    replacement.advance_generation = true;
    replacement.epoch = harness.epoch();
    const auto second = harness.coordinator.register_critic(replacement);
    Harness::require(second.ok(), second.status());
    std::printf("critic generation advanced %llu -> %llu\n",
                static_cast<unsigned long long>(first.value().generation.value()),
                static_cast<unsigned long long>(second.value().generation.value()));

    const ReviewRecord review = harness.declare(target, default_review_spec());
    AssignCriticsRequest request;
    request.review = review.id;
    request.review_generation = review.generation;
    AssignCriticsRequest::Assignment assignment;
    assignment.critic = first.value().id;
    assignment.critic_generation = first.value().generation;
    assignment.worker = WorkerId::from_value(1);
    assignment.worker_boot = WorkerBootId::from_value(1);
    assignment.role = CriticRole::GeneralCritic;
    request.assignments.push_back(assignment);
    request.epoch = harness.epoch();
    const auto rejected = harness.coordinator.assign_critics(request);
    Harness::require(!rejected.ok(), "a retired critic generation must not be assignable");
    std::printf("assignment of the retired generation rejected with %s\n", to_string(rejected.code()));
    return rejected.code() == ErrorCode::StaleCriticGeneration ? 0 : 1;
}

// Critic Fabric example — durable review state and conservative recovery.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <string>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/platform.hpp"
#include "example_support.hpp"

int main(int argc, char** argv) {
    using namespace critic_fabric;
    using namespace example;
    platform::configure_process_for_tests();
    banner("coordinator restart and recovery");

    const std::string path = argc > 1 ? argv[1] : std::string("critic_fabric_example_recovery.journal");

    TargetId target_id{};
    ReviewId review_id{};
    CoordinatorEpoch first_epoch{};
    {
        Harness harness(true, path);
        first_epoch = harness.epoch();
        const TargetRecord target = harness.make_target("review in flight when the coordinator stops");
        target_id = target.id;
        const ReviewRecord review = harness.declare(target, default_review_spec());
        review_id = review.id;
        const Harness::Critic critic = harness.make_critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
        const auto assignments =
            harness.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
        harness.publish(assignments.front(), target, critic, SyntheticBehavior::Pass);
        std::printf("first coordinator epoch %llu wrote durable state\n",
                    static_cast<unsigned long long>(first_epoch.value()));
        harness.coordinator.stop();
    }

    {
        Harness restarted(true, path);
        const CoordinatorEpoch second_epoch = restarted.epoch();
        std::printf("restarted coordinator epoch %llu (advanced: %d)\n",
                    static_cast<unsigned long long>(second_epoch.value()),
                    second_epoch.value() > first_epoch.value() ? 1 : 0);
        const RecoveryReport report = restarted.coordinator.recovery_report();
        std::printf("recovered reviews %u, moved to revalidation %u, dynamic authority cleared %u\n",
                    report.reviews_recovered, report.reviews_moved_to_revalidation,
                    report.dynamic_authority_cleared);

        const auto snapshot = restarted.coordinator.query_review(review_id);
        Harness::require(snapshot.ok(), snapshot.status());
        std::printf("review %llu is now %s with authority_current=%d\n",
                    static_cast<unsigned long long>(review_id.value()),
                    to_string(snapshot.value().review.state),
                    snapshot.value().review.authority_current ? 1 : 0);
        const auto target = restarted.coordinator.query_target(target_id);
        Harness::require(target.ok(), target.status());
        Harness::require(snapshot.value().review.state == ReviewState::RevalidationRequired,
                         "an in-flight review must require revalidation after recovery");
        std::remove(path.c_str());
    }
    return 0;
}

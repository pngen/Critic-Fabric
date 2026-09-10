// Critic Fabric — the authority binding every critic-originated message carries.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_AUTHORITY_HPP
#define CRITIC_FABRIC_AUTHORITY_HPP

#include <string>

#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"

namespace critic_fabric {

// A single value object carrying every identity a critic-originated message must
// present. The coordinator validates all of them against current state before
// any mutation; a message that fails any fence is rejected without side effects.
struct AuthorityContext {
    CoordinatorEpoch epoch{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    ReviewId review{};
    ReviewGeneration review_generation{};
    TargetId target{};
    TargetGeneration target_generation{};
    RequestId request{};

    [[nodiscard]] bool is_complete() const noexcept {
        return epoch.valid() && worker.valid() && worker_boot.valid() && critic.valid() &&
               critic_generation.valid() && review.valid() && review_generation.valid() &&
               target.valid() && target_generation.valid() && request.valid();
    }

    [[nodiscard]] std::string to_string() const;
};

// Identifies one physical worker incarnation. Two processes can never share a
// boot identity; a restart always produces a fresh one.
struct WorkerIncarnation {
    WorkerId id{};
    WorkerBootId boot{};
    CoordinatorEpoch epoch{};

    friend bool operator==(const WorkerIncarnation& a, const WorkerIncarnation& b) noexcept {
        return a.id == b.id && a.boot == b.boot && a.epoch == b.epoch;
    }
    friend bool operator!=(const WorkerIncarnation& a, const WorkerIncarnation& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const WorkerIncarnation& a, const WorkerIncarnation& b) noexcept {
        if (a.id != b.id) {
            return a.id < b.id;
        }
        return a.boot < b.boot;
    }
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_AUTHORITY_HPP

// Critic Fabric — critic identity, capability, and process incarnation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_CRITIC_HPP
#define CRITIC_FABRIC_CRITIC_HPP

#include <cstdint>
#include <set>
#include <string>

#include "critic_fabric/digest.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

// What a critic declares it is able to do. Capability declarations gate
// eligibility; they are never treated as evidence of quality.
struct CriticCapability {
    std::set<CriticRole> roles;
    std::set<TargetClass> target_classes;
    std::set<FindingCategory> finding_categories;
    std::string specialization;
    std::string implementation_version;
    Digest capability_evidence{};
    bool deterministic_output = true;
    bool supports_challenge_response = false;

    [[nodiscard]] bool declares_role(CriticRole role) const { return roles.count(role) != 0; }
    [[nodiscard]] bool declares_target_class(TargetClass value) const {
        return target_classes.count(value) != 0;
    }
    [[nodiscard]] bool declares_category(FindingCategory value) const {
        return finding_categories.count(value) != 0;
    }
};

// A durable registration of a logical critic at an exact generation.
struct CriticRegistration {
    CriticId id{};
    CriticGeneration generation = CriticGeneration::first();
    CriticCapability capability;
    ProducerId registrant{};
    std::string provenance;
    Compatibility compat;
    std::uint64_t registered_order = 0;
};

// A physical worker incarnation. Restarting a worker always produces a fresh
// boot identity; authority is never inherited across a process boundary.
struct WorkerRecord {
    WorkerId id{};
    WorkerBootId boot{};
    CoordinatorEpoch epoch{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    ProducerId host{};
    std::string process_label;
    bool ready = false;
    bool healthy = false;
    bool authoritative = false;
    // Monotonic coordinator-assigned ordering across every worker incarnation.
    // The incumbent for a logical critic is the incarnation with the highest
    // boot_order, so a second process claiming the same critic identity can
    // never silently inflate participation.
    std::uint64_t boot_order = 0;
    std::uint64_t readiness_order = 0;
    Digest readiness_digest{};
};

struct RegisterCriticRequest {
    CriticId id{};
    CriticCapability capability;
    ProducerId registrant{};
    std::string provenance;
    Compatibility compat;
    bool advance_generation = false;
    CoordinatorEpoch epoch{};
    RequestId request{};
};

struct RegisterWorkerRequest {
    WorkerId id{};
    WorkerBootId boot{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    ProducerId host{};
    std::string process_label;
    CoordinatorEpoch epoch{};
    RequestId request{};
};

// Readiness is dynamic, process-local, and never durable. A recovered
// coordinator conservatively clears it.
struct DeclareReadinessRequest {
    WorkerId id{};
    WorkerBootId boot{};
    CriticId critic{};
    CriticGeneration critic_generation{};
    bool ready = false;
    bool healthy = false;
    Digest capability_evidence{};
    CoordinatorEpoch epoch{};
    RequestId request{};
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_CRITIC_HPP

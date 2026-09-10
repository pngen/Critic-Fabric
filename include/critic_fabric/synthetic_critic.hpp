// Critic Fabric — deterministic synthetic reference critics.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These critics are SYNTHETIC. They exercise the governance contracts of the
// runtime deterministically and without any external model service. They are
// reference workers, not a measure of real model evaluation quality.
#ifndef CRITIC_FABRIC_SYNTHETIC_CRITIC_HPP
#define CRITIC_FABRIC_SYNTHETIC_CRITIC_HPP

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "critic_fabric/critic.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/ids.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

// A synchronization gate. Holders block in wait() until release() is called.
// This is a real synchronization primitive, never a timed wait: tests control
// ordering explicitly instead of relying on a delay.
class Gate {
public:
    explicit Gate(bool open = true) : open_(open) {}

    void hold() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return open_; });
    }

    [[nodiscard]] bool open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    bool open_;
};

enum class SyntheticBehavior : std::uint32_t {
    Pass = 1,
    Fail = 2,
    Warn = 3,
    Abstain = 4,
    Unknown = 5,
    NotApplicable = 6,
    Inconclusive = 7,
};

const char* to_string(SyntheticBehavior behavior) noexcept;
bool parse_synthetic_behavior(const std::string& text, SyntheticBehavior& out);

struct SyntheticCriticConfig {
    CriticId critic{};
    CriticRole role = CriticRole::GeneralCritic;
    SyntheticBehavior behavior = SyntheticBehavior::Pass;
    FindingCategory category = FindingCategory::Correctness;
    Severity severity = Severity::Critical;
    EvidenceSourceType source_type = EvidenceSourceType::Computation;
    EvidenceProvenance provenance = EvidenceProvenance::Derived;
    std::string specialization = "synthetic-reference";
    std::string implementation_version = "1.0.0";
    bool deterministic_output = true;
    // Optional gate the worker waits on immediately before publishing, so a
    // harness can hold a critic mid-review without a sleep.
    std::shared_ptr<Gate> publish_gate;
};

// The capability document a synthetic critic declares. The coordinator stores
// its digest and requires the same digest at readiness declaration, so a fresh
// process cannot inherit authority from a previous incarnation.
CriticCapability synthetic_critic_capability(const SyntheticCriticConfig& config);
Digest synthetic_capability_evidence(const SyntheticCriticConfig& config);

struct SyntheticReviewOutput {
    FindingOutcome outcome = FindingOutcome::Unknown;
    Severity severity = Severity::Info;
    FindingCategory category = FindingCategory::Other;
    EvidenceSourceType source_type = EvidenceSourceType::Computation;
    EvidenceProvenance provenance = EvidenceProvenance::Derived;
    std::vector<std::uint8_t> evidence_payload;
    std::string explanation;
};

// Purely a function of the configuration, the critic generation, and the target
// content. Two runs over identical inputs produce identical output.
SyntheticReviewOutput run_synthetic_critic(const SyntheticCriticConfig& config,
                                           CriticGeneration generation,
                                           const TargetRecord& target);

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_SYNTHETIC_CRITIC_HPP

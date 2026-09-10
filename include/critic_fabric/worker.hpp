// Critic Fabric — critic worker runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_WORKER_HPP
#define CRITIC_FABRIC_WORKER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "critic_fabric/synthetic_critic.hpp"
#include "critic_fabric/transport.hpp"

namespace critic_fabric {

struct WorkerConfig {
    std::string coordinator_host = "127.0.0.1";
    std::uint16_t coordinator_port = 0;
    CriticId critic{};
    std::string process_label = "critic-worker";
    ProducerId host{};
    RuntimeLimits limits;
    SyntheticCriticConfig critic_config;
    // Number of assignment notices to service before the process exits.
    std::uint32_t max_assignments = 1;
    // When false the worker registers but never declares readiness; useful for
    // proving that a fresh incarnation cannot inherit authority.
    bool declare_readiness = true;
    // When true the worker advances the critic generation at registration.
    bool advance_critic_generation = false;
    // Reuses an explicit boot identity. Only used to prove that a replayed boot
    // identity is rejected; production workers always generate a fresh one.
    WorkerBootId forced_boot{};
    WorkerId forced_worker{};
    bool print_summary = false;
    // Invoked on the worker thread immediately before publication, so an
    // operator tool can announce that the critic is about to publish.
    std::function<void(const CriticAssignment&)> before_publish;
};

// A fresh, process-unique worker boot identity. Derived from the process id, a
// monotonic counter, and the steady clock so two incarnations can never collide.
WorkerBootId generate_worker_boot_id(const std::string& process_label);

class CriticWorker {
public:
    explicit CriticWorker(WorkerConfig config);
    ~CriticWorker();

    CriticWorker(const CriticWorker&) = delete;
    CriticWorker& operator=(const CriticWorker&) = delete;

    // Connects, registers the critic and worker identity, declares readiness,
    // services assignment notices, and returns.
    Status run();

    [[nodiscard]] WorkerId worker_id() const noexcept { return worker_id_; }
    [[nodiscard]] WorkerBootId boot_id() const noexcept { return boot_id_; }
    [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] CriticGeneration critic_generation() const noexcept { return critic_generation_; }
    [[nodiscard]] std::uint32_t serviced() const noexcept { return serviced_.load(); }

private:
    Status service_assignment(const CriticAssignment& assignment);

    WorkerConfig config_;
    CoordinatorClient client_;
    WorkerId worker_id_{};
    WorkerBootId boot_id_{};
    CoordinatorEpoch epoch_{};
    CriticGeneration critic_generation_{};
    std::atomic<std::uint32_t> serviced_{0};
};

// Serves one assignment notice that arrived on the given client. Used by the
// optional accelerator-backed worker so the CUDA path shares the governance
// code rather than duplicating it.
Status publish_synthetic_assignment(CoordinatorClient& client, const WorkerConfig& config,
                                    const CriticAssignment& assignment, CoordinatorEpoch epoch,
                                    WorkerId worker_id, WorkerBootId boot_id,
                                    CriticGeneration critic_generation,
                                    const std::shared_ptr<Gate>& gate);

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_WORKER_HPP

// Critic Fabric — critic worker process.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Usage: critic_worker --port N --critic ID [--host H] [--role ROLE]
//                      [--behavior pass|fail|warn|abstain|unknown|not_applicable|inconclusive]
//                      [--category CATEGORY] [--severity SEVERITY]
//                      [--max N] [--label TEXT] [--no-readiness]
//                      [--advance-generation] [--force-boot ID] [--force-worker ID]
//                      [--hold-before-publish]
//
// The synthetic reference critic mode is the default. --hold-before-publish
// makes the worker stop immediately before publication and wait for a line on
// standard input, which lets a supervising process kill it at exactly the
// interesting moment without relying on a delay.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "critic_fabric/platform.hpp"
#include "critic_fabric/worker.hpp"

namespace {

struct Options {
    critic_fabric::WorkerConfig config;
    bool hold_before_publish = false;
};

bool next_value(int argc, char** argv, int& index, const char* name, std::string& out) {
    if (index + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        return false;
    }
    out = argv[++index];
    return true;
}

bool parse_options(int argc, char** argv, Options& options) {
    using namespace critic_fabric;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        std::string value;
        if (argument == "--port") {
            if (!next_value(argc, argv, i, "--port", value)) return false;
            options.config.coordinator_port = static_cast<std::uint16_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (argument == "--host") {
            if (!next_value(argc, argv, i, "--host", value)) return false;
            options.config.coordinator_host = value;
        } else if (argument == "--critic") {
            if (!next_value(argc, argv, i, "--critic", value)) return false;
            options.config.critic = CriticId::from_value(std::strtoull(value.c_str(), nullptr, 10));
        } else if (argument == "--role") {
            if (!next_value(argc, argv, i, "--role", value)) return false;
            CriticRole role{};
            if (!parse_critic_role(value, role)) {
                std::fprintf(stderr, "unknown critic role: %s\n", value.c_str());
                return false;
            }
            options.config.critic_config.role = role;
        } else if (argument == "--behavior") {
            if (!next_value(argc, argv, i, "--behavior", value)) return false;
            SyntheticBehavior behavior{};
            if (!parse_synthetic_behavior(value, behavior)) {
                std::fprintf(stderr, "unknown synthetic behavior: %s\n", value.c_str());
                return false;
            }
            options.config.critic_config.behavior = behavior;
        } else if (argument == "--category") {
            if (!next_value(argc, argv, i, "--category", value)) return false;
            FindingCategory category{};
            if (!parse_finding_category(value, category)) {
                std::fprintf(stderr, "unknown finding category: %s\n", value.c_str());
                return false;
            }
            options.config.critic_config.category = category;
        } else if (argument == "--severity") {
            if (!next_value(argc, argv, i, "--severity", value)) return false;
            Severity severity{};
            if (!parse_severity(value, severity)) {
                std::fprintf(stderr, "unknown severity: %s\n", value.c_str());
                return false;
            }
            options.config.critic_config.severity = severity;
        } else if (argument == "--max") {
            if (!next_value(argc, argv, i, "--max", value)) return false;
            options.config.max_assignments = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (argument == "--label") {
            if (!next_value(argc, argv, i, "--label", value)) return false;
            options.config.process_label = value;
        } else if (argument == "--force-boot") {
            if (!next_value(argc, argv, i, "--force-boot", value)) return false;
            options.config.forced_boot = WorkerBootId::from_value(std::strtoull(value.c_str(), nullptr, 10));
        } else if (argument == "--force-worker") {
            if (!next_value(argc, argv, i, "--force-worker", value)) return false;
            options.config.forced_worker = WorkerId::from_value(std::strtoull(value.c_str(), nullptr, 10));
        } else if (argument == "--no-readiness") {
            options.config.declare_readiness = false;
        } else if (argument == "--advance-generation") {
            options.config.advance_critic_generation = true;
        } else if (argument == "--hold-before-publish") {
            options.hold_before_publish = true;
        } else if (argument == "--help") {
            std::printf("critic_worker --port N --critic ID [options]\n");
            return false;
        } else {
            std::fprintf(stderr, "unrecognised argument: %s\n", argument.c_str());
            return false;
        }
    }
    if (options.config.coordinator_port == 0 || !options.config.critic.valid()) {
        std::fprintf(stderr, "--port and --critic are required\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    critic_fabric::platform::configure_process_for_tests();
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }

    using namespace critic_fabric;
    std::shared_ptr<Gate> gate;
    if (options.hold_before_publish) {
        gate = std::make_shared<Gate>(false);
        options.config.critic_config.publish_gate = gate;
        options.config.before_publish = [](const CriticAssignment& assignment) {
            std::printf("CRITIC_WORKER_HOLDING assignment=%llu review=%llu\n",
                        static_cast<unsigned long long>(assignment.id.value()),
                        static_cast<unsigned long long>(assignment.review.value()));
            std::fflush(stdout);
        };
        // A supervising process releases the hold by writing any line to this
        // process's standard input. Killing the process instead is exactly the
        // worker-death case the runtime must fence.
        std::thread([gate] {
            char buffer[64];
            if (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
                gate->release();
            }
        }).detach();
    }

    CriticWorker worker(options.config);
    Status status = worker.run();
    if (!status.ok()) {
        std::fprintf(stderr, "CRITIC_WORKER_ERROR %s %s\n", to_string(status.code()), status.detail().c_str());
        return 1;
    }
    std::printf("CRITIC_WORKER_DONE critic=%llu worker=%llu boot=%llu generation=%llu serviced=%u\n",
                static_cast<unsigned long long>(options.config.critic.value()),
                static_cast<unsigned long long>(worker.worker_id().value()),
                static_cast<unsigned long long>(worker.boot_id().value()),
                static_cast<unsigned long long>(worker.critic_generation().value()),
                static_cast<unsigned>(worker.serviced()));
    std::fflush(stdout);
    return 0;
}

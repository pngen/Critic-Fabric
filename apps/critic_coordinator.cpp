// Critic Fabric — coordinator process.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Usage: critic_coordinator [--port N] [--persistence PATH] [--label TEXT]
//
// Prints one readiness line to stdout so a supervising process learns the bound
// port without polling:
//   CRITIC_COORDINATOR_READY port=<n> epoch=<e> persistence=<path|memory> ...

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/platform.hpp"
#include "critic_fabric/transport.hpp"

namespace {

struct Options {
    std::uint16_t port = 0;
    std::string persistence;
    std::string label = "coordinator";
};

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--port" && i + 1 < argc) {
            options.port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (argument == "--persistence" && i + 1 < argc) {
            options.persistence = argv[++i];
        } else if (argument == "--label" && i + 1 < argc) {
            options.label = argv[++i];
        } else if (argument == "--help") {
            std::printf("critic_coordinator [--port N] [--persistence PATH] [--label TEXT]\n");
            return false;
        } else {
            std::fprintf(stderr, "unrecognised argument: %s\n", argument.c_str());
            return false;
        }
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

    CoordinatorConfig config;
    config.persistence_path = options.persistence;
    config.process_label = options.label;

    Coordinator coordinator(config);
    Status status = coordinator.start();
    if (!status.ok()) {
        std::fprintf(stderr, "coordinator start failed: %s\n", status.to_string().c_str());
        return 1;
    }

    std::mutex shutdown_mutex;
    std::condition_variable shutdown_condition;
    bool shutdown_requested = false;
    const auto request_shutdown = [&] {
        {
            std::lock_guard<std::mutex> lock(shutdown_mutex);
            shutdown_requested = true;
        }
        shutdown_condition.notify_all();
    };

    ServerConfig server_config;
    server_config.port = options.port;
    server_config.limits = coordinator.limits();
    server_config.shutdown_requested = request_shutdown;
    CoordinatorServer server(coordinator, server_config);
    status = server.start();
    if (!status.ok()) {
        std::fprintf(stderr, "server start failed: %s\n", status.to_string().c_str());
        return 1;
    }

    const RecoveryReport report = coordinator.recovery_report();
    std::printf("CRITIC_COORDINATOR_READY port=%u epoch=%llu persistence=%s recovered=%u revalidation=%u\n",
                static_cast<unsigned>(server.bound_port()),
                static_cast<unsigned long long>(report.epoch.value()),
                options.persistence.empty() ? "memory" : options.persistence.c_str(),
                static_cast<unsigned>(report.durable_state_recovered ? 1u : 0u),
                static_cast<unsigned>(report.reviews_moved_to_revalidation));
    std::fflush(stdout);

    // Two shutdown paths, both explicit and neither polling: a supervising
    // process may close the standard input pipe or write "stop" to it, or it may
    // send an administrative Shutdown message over the control connection. The
    // main thread waits on a condition variable rather than spinning.
    std::thread input_thread([&request_shutdown] {
        char buffer[256];
        while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
            if (std::strncmp(buffer, "stop", 4) == 0) {
                break;
            }
        }
        request_shutdown();
    });
    input_thread.detach();

    {
        std::unique_lock<std::mutex> lock(shutdown_mutex);
        shutdown_condition.wait(lock, [&shutdown_requested] { return shutdown_requested; });
    }

    server.stop();
    coordinator.stop();
    std::printf("CRITIC_COORDINATOR_STOPPED\n");
    std::fflush(stdout);
    std::fflush(stderr);
    // The detached input thread may still be blocked on a pipe read; exit
    // immediately rather than waiting on a handle the supervisor controls.
    std::_Exit(0);
}

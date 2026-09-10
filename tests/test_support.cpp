// Critic Fabric — test harness implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "critic_fabric/platform.hpp"

namespace cftest {
namespace {

struct Entry {
    std::string name;
    TestFunction function;
};

std::vector<Entry>& registry() {
    static std::vector<Entry> entries;
    return entries;
}

std::string g_current_test;
int g_failures = 0;

struct RequirementFailure {
    std::string message;
};

}  // namespace

Registrar::Registrar(const char* name, TestFunction function) {
    registry().push_back(Entry{name, function});
}

void record_failure(const char* file, int line, const std::string& message) {
    g_failures += 1;
    std::fprintf(stderr, "FAIL %s: %s:%d: %s\n", g_current_test.c_str(), file, line, message.c_str());
    std::fflush(stderr);
}

void record_requirement(const char* file, int line, const std::string& message) {
    record_failure(file, line, message);
    throw RequirementFailure{message};
}

int run_all(int argc, char** argv) {
    critic_fabric::platform::configure_process_for_tests();
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--filter=", 9) == 0) {
            filter = argv[i] + 9;
        }
    }

    int ran = 0;
    for (const Entry& entry : registry()) {
        if (!filter.empty() && entry.name.find(filter) == std::string::npos) {
            continue;
        }
        g_current_test = entry.name;
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", entry.name.c_str());
        std::fflush(stdout);
        try {
            entry.function();
        } catch (const RequirementFailure&) {
            // Already recorded.
        } catch (const std::exception& error) {
            record_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + error.what());
        } catch (...) {
            record_failure(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        ran += 1;
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", entry.name.c_str());
        std::fflush(stdout);
    }

    std::printf("\n%d test(s) run, %d failed\n", ran, g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}

TempDirectory::TempDirectory(const std::string& tag) {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    const std::uint64_t nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    path_ = (base / ("critic_fabric_test_" + tag + "_" + std::to_string(nonce))).string();
    std::error_code error;
    std::filesystem::create_directories(path_, error);
}

TempDirectory::~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

std::string TempDirectory::file(const std::string& name) const {
    return (std::filesystem::path(path_) / name).string();
}

}  // namespace cftest

int main(int argc, char** argv) { return cftest::run_all(argc, argv); }

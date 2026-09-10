// Critic Fabric — minimal, dependency-free test harness.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_TEST_SUPPORT_HPP
#define CRITIC_FABRIC_TEST_SUPPORT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "critic_fabric/coordinator.hpp"

namespace cftest {

using TestFunction = void (*)();

struct Registrar {
    Registrar(const char* name, TestFunction function);
};

// Records a failure and continues. CF_REQUIRE aborts only the current test.
void record_failure(const char* file, int line, const std::string& message);
void record_requirement(const char* file, int line, const std::string& message);

int run_all(int argc, char** argv);

// Deterministic pseudo random source for seeded randomized tests. It is seeded
// explicitly and its parameters are printed on failure so any failure is
// reproducible.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

    std::uint64_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return state_;
    }

    std::uint32_t below(std::uint32_t bound) {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
    }

    [[nodiscard]] std::uint64_t seed() const { return seed_; }

private:
    std::uint64_t state_;
    std::uint64_t seed_ = 0;
};

// A temporary directory under the system temporary location, removed on
// destruction so no disposable artifact survives a run.
class TempDirectory {
public:
    explicit TempDirectory(const std::string& tag);
    ~TempDirectory();

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string file(const std::string& name) const;

private:
    std::string path_;
};

}  // namespace cftest

#define CF_TEST(name)                                                        \
    static void name();                                                      \
    static ::cftest::Registrar cf_registrar_##name(#name, name);             \
    static void name()

#define CF_CHECK(condition)                                                  \
    do {                                                                     \
        if (!(condition)) {                                                  \
            ::cftest::record_failure(__FILE__, __LINE__,                     \
                                     "check failed: " #condition);           \
        }                                                                    \
    } while (false)

#define CF_CHECK_MSG(condition, message)                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            ::cftest::record_failure(__FILE__, __LINE__,                     \
                                     std::string("check failed: " #condition ": ") + (message)); \
        }                                                                    \
    } while (false)

// Aborts only the current test by throwing, so it is valid inside helper
// functions that must return a value.
#define CF_REQUIRE(condition)                                                \
    do {                                                                     \
        if (!(condition)) {                                                  \
            ::cftest::record_requirement(__FILE__, __LINE__,                 \
                                         "requirement failed: " #condition); \
        }                                                                    \
    } while (false)

#define CF_CHECK_EQ(left, right)                                             \
    do {                                                                     \
        const auto cf_left = (left);                                         \
        const auto cf_right = (right);                                       \
        if (!(cf_left == cf_right)) {                                        \
            ::cftest::record_failure(__FILE__, __LINE__,                     \
                                     std::string("check failed: " #left " == " #right)); \
        }                                                                    \
    } while (false)

#endif  // CRITIC_FABRIC_TEST_SUPPORT_HPP

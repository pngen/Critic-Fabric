// Critic Fabric — strongly typed identities and generations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_IDS_HPP
#define CRITIC_FABRIC_IDS_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace critic_fabric {

// Strongly typed identity. Distinct tags produce distinct types that cannot be
// silently interchanged, so no authority domain collapses into a bare integer.
template <typename Tag>
class StrongId {
public:
    using value_type = std::uint64_t;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(value_type v) noexcept : value_(v) {}

    static constexpr StrongId from_value(value_type v) noexcept { return StrongId(v); }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value_ == b.value_; }
    friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return a.value_ != b.value_; }
    friend constexpr bool operator<(StrongId a, StrongId b) noexcept { return a.value_ < b.value_; }
    friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return a.value_ > b.value_; }
    friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return a.value_ <= b.value_; }
    friend constexpr bool operator>=(StrongId a, StrongId b) noexcept { return a.value_ >= b.value_; }

private:
    value_type value_ = 0;
};

// Strongly typed generation counter. Generation zero means "not established".
// Generation one is the first established generation.
template <typename Tag>
class StrongGeneration {
public:
    using value_type = std::uint64_t;

    constexpr StrongGeneration() noexcept = default;
    explicit constexpr StrongGeneration(value_type v) noexcept : value_(v) {}

    static constexpr StrongGeneration first() noexcept { return StrongGeneration(1); }
    static constexpr StrongGeneration from_value(value_type v) noexcept { return StrongGeneration(v); }

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    // True when advancing would overflow the representable generation space.
    [[nodiscard]] constexpr bool advance_would_overflow() const noexcept {
        return value_ == (std::numeric_limits<value_type>::max)();
    }

    // Callers must check advance_would_overflow() first; the failure is reported
    // as a typed resource-limit rejection rather than wrapping silently.
    [[nodiscard]] constexpr StrongGeneration advance() const noexcept {
        return StrongGeneration(value_ + 1);
    }

    [[nodiscard]] constexpr bool is_newer_than(StrongGeneration other) const noexcept {
        return value_ > other.value_;
    }

    friend constexpr bool operator==(StrongGeneration a, StrongGeneration b) noexcept {
        return a.value_ == b.value_;
    }
    friend constexpr bool operator!=(StrongGeneration a, StrongGeneration b) noexcept {
        return a.value_ != b.value_;
    }
    friend constexpr bool operator<(StrongGeneration a, StrongGeneration b) noexcept {
        return a.value_ < b.value_;
    }
    friend constexpr bool operator>(StrongGeneration a, StrongGeneration b) noexcept {
        return a.value_ > b.value_;
    }
    friend constexpr bool operator<=(StrongGeneration a, StrongGeneration b) noexcept {
        return a.value_ <= b.value_;
    }
    friend constexpr bool operator>=(StrongGeneration a, StrongGeneration b) noexcept {
        return a.value_ >= b.value_;
    }

private:
    value_type value_ = 0;
};

#define CRITIC_FABRIC_DEFINE_ID(name)                       \
    struct name##Tag {};                                    \
    using name = StrongId<name##Tag>;

#define CRITIC_FABRIC_DEFINE_GEN(name)                      \
    struct name##Tag {};                                    \
    using name = StrongGeneration<name##Tag>;

CRITIC_FABRIC_DEFINE_ID(ReviewRequestId)
CRITIC_FABRIC_DEFINE_ID(ReviewId)
CRITIC_FABRIC_DEFINE_ID(TargetId)
CRITIC_FABRIC_DEFINE_ID(CriticId)
CRITIC_FABRIC_DEFINE_ID(CriticAssignmentId)
CRITIC_FABRIC_DEFINE_ID(FindingId)
CRITIC_FABRIC_DEFINE_ID(EvidenceId)
CRITIC_FABRIC_DEFINE_ID(ChallengeId)
CRITIC_FABRIC_DEFINE_ID(RebuttalId)
CRITIC_FABRIC_DEFINE_ID(DecisionId)
CRITIC_FABRIC_DEFINE_ID(WorkerId)
CRITIC_FABRIC_DEFINE_ID(WorkerBootId)
CRITIC_FABRIC_DEFINE_ID(ProducerId)
CRITIC_FABRIC_DEFINE_ID(RequestId)

CRITIC_FABRIC_DEFINE_GEN(ReviewGeneration)
CRITIC_FABRIC_DEFINE_GEN(TargetGeneration)
CRITIC_FABRIC_DEFINE_GEN(CriticGeneration)
CRITIC_FABRIC_DEFINE_GEN(FindingGeneration)
CRITIC_FABRIC_DEFINE_GEN(EvidenceGeneration)
CRITIC_FABRIC_DEFINE_GEN(ChallengeGeneration)
CRITIC_FABRIC_DEFINE_GEN(DecisionGeneration)
CRITIC_FABRIC_DEFINE_GEN(CoordinatorEpoch)
CRITIC_FABRIC_DEFINE_GEN(AttemptGeneration)

#undef CRITIC_FABRIC_DEFINE_ID
#undef CRITIC_FABRIC_DEFINE_GEN

// Ordered identifier for critic roles, used only for deterministic maps.
struct CriticRoleId {
    CriticId critic{};
    CriticRoleId() = default;
    explicit CriticRoleId(CriticId c) noexcept : critic(c) {}
    friend bool operator==(CriticRoleId a, CriticRoleId b) noexcept { return a.critic == b.critic; }
    friend bool operator<(CriticRoleId a, CriticRoleId b) noexcept { return a.critic < b.critic; }
};

}  // namespace critic_fabric

namespace std {

#define CRITIC_FABRIC_HASH_ID(name)                                        \
    template <>                                                            \
    struct hash<critic_fabric::name> {                                     \
        size_t operator()(const critic_fabric::name& v) const noexcept {   \
            return std::hash<std::uint64_t>{}(v.value());                  \
        }                                                                  \
    };

CRITIC_FABRIC_HASH_ID(ReviewRequestId)
CRITIC_FABRIC_HASH_ID(ReviewId)
CRITIC_FABRIC_HASH_ID(TargetId)
CRITIC_FABRIC_HASH_ID(CriticId)
CRITIC_FABRIC_HASH_ID(CriticAssignmentId)
CRITIC_FABRIC_HASH_ID(FindingId)
CRITIC_FABRIC_HASH_ID(EvidenceId)
CRITIC_FABRIC_HASH_ID(ChallengeId)
CRITIC_FABRIC_HASH_ID(RebuttalId)
CRITIC_FABRIC_HASH_ID(DecisionId)
CRITIC_FABRIC_HASH_ID(WorkerId)
CRITIC_FABRIC_HASH_ID(WorkerBootId)
CRITIC_FABRIC_HASH_ID(ProducerId)
CRITIC_FABRIC_HASH_ID(RequestId)

CRITIC_FABRIC_HASH_ID(ReviewGeneration)
CRITIC_FABRIC_HASH_ID(TargetGeneration)
CRITIC_FABRIC_HASH_ID(CriticGeneration)
CRITIC_FABRIC_HASH_ID(FindingGeneration)
CRITIC_FABRIC_HASH_ID(EvidenceGeneration)
CRITIC_FABRIC_HASH_ID(ChallengeGeneration)
CRITIC_FABRIC_HASH_ID(DecisionGeneration)
CRITIC_FABRIC_HASH_ID(CoordinatorEpoch)
CRITIC_FABRIC_HASH_ID(AttemptGeneration)

#undef CRITIC_FABRIC_HASH_ID

}  // namespace std

#endif  // CRITIC_FABRIC_IDS_HPP

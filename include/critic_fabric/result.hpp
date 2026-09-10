// Critic Fabric — typed outcomes. Distinct failures stay distinct.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_RESULT_HPP
#define CRITIC_FABRIC_RESULT_HPP

#include <cstdint>
#include <string>
#include <utility>

namespace critic_fabric {

// Every materially different rejection has its own code. Nothing collapses into
// a bare false that an operator would have to reverse-engineer from logs.
enum class ErrorCode : std::uint32_t {
    Ok = 0,

    // Authority fences.
    StaleReviewGeneration = 100,
    StaleTargetGeneration = 101,
    StaleCriticGeneration = 102,
    StaleWorkerBoot = 103,
    StaleCoordinatorEpoch = 104,
    CriticNotAuthoritative = 105,
    CriticNotRegistered = 106,
    CriticCapabilityMismatch = 107,
    WorkerNotRegistered = 108,
    DuplicateCriticIdentity = 109,
    DuplicateWorkerIdentity = 110,
    CriticGenerationRegressed = 111,
    InvalidIdentity = 112,

    // Target binding.
    TargetUnknown = 200,
    TargetNotReviewable = 201,
    TargetMismatch = 202,
    TargetGenerationRegressed = 203,
    TargetSuperseded = 204,
    TargetDigestMismatch = 205,

    // Review lifecycle.
    ReviewUnknown = 300,
    ReviewAlreadyExists = 301,
    ReviewCancelled = 302,
    ReviewSuperseded = 303,
    ReviewNotCommittable = 304,
    InvalidStateTransition = 305,
    ReviewNotOpen = 306,
    ReviewClosed = 307,

    // Findings and evidence.
    FindingUnknown = 400,
    MalformedFinding = 401,
    MalformedEvidence = 402,
    DuplicateFinding = 403,
    DuplicateEvidence = 404,
    ConflictingFinding = 405,
    WrongTargetFinding = 406,
    EvidenceNotCurrent = 407,
    EvidenceIntegrityFailed = 408,
    EvidenceDigestMismatch = 409,
    EvidenceProvenanceUnknown = 410,
    FindingSuperseded = 411,

    // Policy and decision.
    InsufficientEvidence = 500,
    RequiredCriticUnavailable = 501,
    RequiredCriticFailed = 502,
    QuorumNotReached = 503,
    DisagreementUnresolved = 504,
    HardPredicateFailed = 505,
    PolicyRejected = 506,
    ResultAlreadyCommitted = 507,
    ConflictingReviewCommit = 508,
    DecisionUnknown = 509,
    StaleDecisionGeneration = 510,
    RevalidationRequired = 511,

    // Challenge and rebuttal.
    ChallengeUnknown = 600,
    InvalidChallenge = 601,
    StaleChallenge = 602,
    InvalidRebuttal = 603,
    StaleRebuttal = 604,
    ChallengeLimitExceeded = 605,
    ChallengeDisabled = 606,

    // Persistence.
    PersistenceCorruption = 700,
    PersistenceTruncated = 701,
    PersistenceTrailingData = 702,
    PersistenceUnsupportedVersion = 703,
    PersistenceBadMagic = 704,
    PersistenceIoError = 705,
    PersistenceChecksumMismatch = 706,
    PersistenceUnavailable = 707,

    // Protocol.
    ProtocolError = 800,
    ProtocolBadMagic = 801,
    ProtocolBadVersion = 802,
    ProtocolBadChecksum = 803,
    ProtocolTruncated = 804,
    ProtocolOversized = 805,
    ProtocolTrailingGarbage = 806,
    ProtocolInvalidEnum = 807,
    ProtocolUnknownMessageType = 808,
    ProtocolConnectionClosed = 809,
    ProtocolTimeout = 810,

    // Resources and internals.
    ResourceLimitExceeded = 900,
    ArithmeticOverflow = 901,
    Unsupported = 902,
    InternalError = 903,
};

const char* to_string(ErrorCode code) noexcept;

// True when the rejection is a fence that a caller may safely retry after
// refreshing authority. Purely descriptive; it grants nothing.
bool is_retryable_fence(ErrorCode code) noexcept;

class Status {
public:
    Status() = default;
    Status(ErrorCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

    static Status success() { return Status(); }
    static Status error(ErrorCode code, std::string detail) { return Status(code, std::move(detail)); }

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
    [[nodiscard]] std::string to_string() const;

    explicit operator bool() const noexcept { return ok(); }

private:
    ErrorCode code_ = ErrorCode::Ok;
    std::string detail_;
};

template <typename T>
class Result {
public:
    Result(T value) : status_(), value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Status status) : status_(std::move(status)), value_() {}   // NOLINT(google-explicit-constructor)
    Result(ErrorCode code, std::string detail) : status_(code, std::move(detail)), value_() {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] ErrorCode code() const noexcept { return status_.code(); }
    [[nodiscard]] const std::string& detail() const noexcept { return status_.detail(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

    // Precondition: ok(). Calling value() on a failed result is a programming
    // error; callers must check ok() first.
    //
    // Only lvalue access is provided. Binding a reference into a temporary
    // result would dangle as soon as the temporary dies, for example in
    // "for (const auto& item : make_result().value())", where the range-for
    // extends the reference but not the enclosing temporary. That form is a
    // compile error instead of a latent use-after-scope.
    T& value() & noexcept { return value_; }
    const T& value() const& noexcept { return value_; }
    T&& value() && = delete;

    T* operator->() noexcept { return &value_; }
    const T* operator->() const noexcept { return &value_; }
    T& operator*() & noexcept { return value_; }
    const T& operator*() const& noexcept { return value_; }

private:
    Status status_;
    T value_;
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_RESULT_HPP

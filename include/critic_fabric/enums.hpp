// Critic Fabric — closed enumerations with stable numeric numbering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_ENUMS_HPP
#define CRITIC_FABRIC_ENUMS_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace critic_fabric {

// Numeric values are part of the persisted and wire format. New values may be
// appended; existing values must never be renumbered.

enum class TargetClass : std::uint32_t {
    Claim = 1,
    Artifact = 2,
    Plan = 3,
    Result = 4,
    ModelOutput = 5,
    ExecutionOutput = 6,
    Configuration = 7,
    PolicyResult = 8,
    DatasetSlice = 9,
    OtherTypedTarget = 10,
};
inline constexpr std::uint32_t kTargetClassMax = 10;

enum class CriticRole : std::uint32_t {
    GeneralCritic = 1,
    FactChecker = 2,
    LogicCritic = 3,
    SafetyCritic = 4,
    PerformanceCritic = 5,
    SecurityCritic = 6,
    CorrectnessVerifier = 7,
    ConsistencyVerifier = 8,
    SchemaVerifier = 9,
    AdversarialCritic = 10,
    DomainSpecialist = 11,
    RebuttalCritic = 12,
};
inline constexpr std::uint32_t kCriticRoleMax = 12;

enum class FindingCategory : std::uint32_t {
    Correctness = 1,
    Consistency = 2,
    Schema = 3,
    Safety = 4,
    Security = 5,
    Performance = 6,
    Logic = 7,
    Factual = 8,
    EvidenceQuality = 9,
    Policy = 10,
    Other = 11,
};
inline constexpr std::uint32_t kFindingCategoryMax = 11;

// Severity is a bounded ordinal. It affects policy thresholds. It never by
// itself establishes truth, and it is never conflated with outcome.
enum class Severity : std::uint32_t {
    Info = 1,
    Low = 2,
    Medium = 3,
    High = 4,
    Critical = 5,
};
inline constexpr std::uint32_t kSeverityMax = 5;

// Outcome is what the critic asserts about the target. Severity is how serious
// the assertion is. The two are independent: a Critical severity finding may be
// unverified, and a Low severity finding may be authoritative.
enum class FindingOutcome : std::uint32_t {
    Pass = 1,
    Fail = 2,
    Warn = 3,
    Abstain = 4,
    Unknown = 5,
    NotApplicable = 6,
    Inconclusive = 7,
};
inline constexpr std::uint32_t kFindingOutcomeMax = 7;

enum class EvidenceSourceType : std::uint32_t {
    Instrumentation = 1,
    TestRun = 2,
    StaticAnalysis = 3,
    ExecutionTrace = 4,
    ExternalReport = 5,
    Computation = 6,
    HumanAssertion = 7,
    Other = 8,
};
inline constexpr std::uint32_t kEvidenceSourceTypeMax = 8;

// Provenance is the only sanctioned classification of where evidence came from.
// UNKNOWN never satisfies a hard verification predicate.
enum class EvidenceProvenance : std::uint32_t {
    Measured = 1,
    Observed = 2,
    Derived = 3,
    Reported = 4,
    Synthetic = 5,
    Reconstructed = 6,
    Unknown = 7,
};
inline constexpr std::uint32_t kEvidenceProvenanceMax = 7;

enum class EvidenceIntegrity : std::uint32_t {
    Verified = 1,
    Unverified = 2,
    Failed = 3,
    Unknown = 4,
};
inline constexpr std::uint32_t kEvidenceIntegrityMax = 4;

enum class FindingState : std::uint32_t {
    Current = 1,
    Superseded = 2,
    Invalidated = 3,
    Challenged = 4,
    Upheld = 5,
    Rebutted = 6,
    Withdrawn = 7,
};
inline constexpr std::uint32_t kFindingStateMax = 7;

enum class TargetSupersession : std::uint32_t {
    Current = 1,
    Superseded = 2,
    Retired = 3,
};
inline constexpr std::uint32_t kTargetSupersessionMax = 3;

enum class ReviewState : std::uint32_t {
    Declared = 1,
    Assigning = 2,
    Assigned = 3,
    Reviewing = 4,
    FindingsReceived = 5,
    Validating = 6,
    Challenged = 7,
    RebuttalPending = 8,
    Evaluating = 9,
    Resolved = 10,
    Committed = 11,
    RevalidationRequired = 12,
    Cancelled = 13,
    Superseded = 14,
    Failed = 15,
    Retired = 16,
};
inline constexpr std::uint32_t kReviewStateMax = 16;

// Every non-Ok disposition is an explicit, representable review outcome.
// Disagreement, tie, and insufficiency are never coerced into PASS or FAIL.
enum class ReviewDisposition : std::uint32_t {
    None = 0,
    Pass = 1,
    Fail = 2,
    Warn = 3,
    Disagreement = 4,
    Tie = 5,
    InsufficientEvidence = 6,
    RequiredCriticFailed = 7,
    QuorumNotReached = 8,
    RevalidationRequired = 9,
    Cancelled = 10,
    Superseded = 11,
    Unknown = 12,
};
inline constexpr std::uint32_t kReviewDispositionMax = 12;

enum class AggregationPolicy : std::uint32_t {
    AllRequiredPass = 1,
    AnyCriticalFail = 2,
    QuorumPass = 3,
    WeightedReview = 4,
    RequiredRolePass = 5,
    MajorityWithConstraints = 6,
    ConsensusThreshold = 7,
    PolicyTable = 8,
};
inline constexpr std::uint32_t kAggregationPolicyMax = 8;

enum class DisagreementPolicy : std::uint32_t {
    ReportAndFail = 1,
    ReportAndWarn = 2,
    ThresholdThenFail = 3,
    Escalate = 4,
};
inline constexpr std::uint32_t kDisagreementPolicyMax = 4;

enum class DuplicatePolicy : std::uint32_t {
    Reject = 1,
    IdempotentIgnore = 2,
    KeepDistinct = 3,
};
inline constexpr std::uint32_t kDuplicatePolicyMax = 3;

enum class ChallengePolicy : std::uint32_t {
    Disabled = 1,
    AllowOneRound = 2,
    AllowBoundedRounds = 3,
};
inline constexpr std::uint32_t kChallengePolicyMax = 3;

enum class ChallengeOutcome : std::uint32_t {
    Unresolved = 1,
    Upheld = 2,
    Modified = 3,
    Superseded = 4,
    Invalidated = 5,
};
inline constexpr std::uint32_t kChallengeOutcomeMax = 5;

// Hard eligibility predicates. They are evaluated before any scoring, ranking,
// or weighting, and a favorable aggregate can never rescue a failed mandatory
// predicate.
enum class PredicateKind : std::uint32_t {
    ExactTargetGenerationMatch = 1,
    TargetDigestMatch = 2,
    RequiredEvidencePresent = 3,
    EvidenceIntegrityVerified = 4,
    EvidenceProvenanceKnown = 5,
    RequiredRolePresent = 6,
    MinimumIndependentCriticIdentities = 7,
    RequiredVerifierPass = 8,
    NoCriticalFailFindings = 9,
    CriticGenerationCurrent = 10,
    WorkerBootCurrent = 11,
    CoordinatorEpochCurrent = 12,
    RequiredCategoriesCovered = 13,
    QuorumSatisfied = 14,
    ConfidenceNotAuthoritative = 15,
};
inline constexpr std::uint32_t kPredicateKindMax = 15;

enum class ConfidenceBasis : std::uint32_t {
    NotProvided = 0,
    DeclaredByCritic = 1,
    DerivedFromEvidenceCount = 2,
};
inline constexpr std::uint32_t kConfidenceBasisMax = 2;

const char* to_string(TargetClass value) noexcept;
const char* to_string(CriticRole value) noexcept;
const char* to_string(FindingCategory value) noexcept;
const char* to_string(Severity value) noexcept;
const char* to_string(FindingOutcome value) noexcept;
const char* to_string(EvidenceSourceType value) noexcept;
const char* to_string(EvidenceProvenance value) noexcept;
const char* to_string(EvidenceIntegrity value) noexcept;
const char* to_string(FindingState value) noexcept;
const char* to_string(TargetSupersession value) noexcept;
const char* to_string(ReviewState value) noexcept;
const char* to_string(ReviewDisposition value) noexcept;
const char* to_string(AggregationPolicy value) noexcept;
const char* to_string(DisagreementPolicy value) noexcept;
const char* to_string(DuplicatePolicy value) noexcept;
const char* to_string(ChallengePolicy value) noexcept;
const char* to_string(ChallengeOutcome value) noexcept;
const char* to_string(PredicateKind value) noexcept;
const char* to_string(ConfidenceBasis value) noexcept;

// Parsing is exact and case-sensitive; an unrecognized name is rejected rather
// than silently defaulted.
bool parse_target_class(std::string_view text, TargetClass& out) noexcept;
bool parse_critic_role(std::string_view text, CriticRole& out) noexcept;
bool parse_finding_category(std::string_view text, FindingCategory& out) noexcept;
bool parse_severity(std::string_view text, Severity& out) noexcept;
bool parse_finding_outcome(std::string_view text, FindingOutcome& out) noexcept;
bool parse_evidence_provenance(std::string_view text, EvidenceProvenance& out) noexcept;
bool parse_evidence_source_type(std::string_view text, EvidenceSourceType& out) noexcept;
bool parse_review_state(std::string_view text, ReviewState& out) noexcept;
bool parse_review_disposition(std::string_view text, ReviewDisposition& out) noexcept;

// True when the numeric value is a defined member of the enumeration.
bool is_valid_target_class(std::uint32_t raw) noexcept;
bool is_valid_critic_role(std::uint32_t raw) noexcept;
bool is_valid_finding_category(std::uint32_t raw) noexcept;
bool is_valid_severity(std::uint32_t raw) noexcept;
bool is_valid_finding_outcome(std::uint32_t raw) noexcept;
bool is_valid_evidence_source_type(std::uint32_t raw) noexcept;
bool is_valid_evidence_provenance(std::uint32_t raw) noexcept;
bool is_valid_evidence_integrity(std::uint32_t raw) noexcept;
bool is_valid_finding_state(std::uint32_t raw) noexcept;
bool is_valid_target_supersession(std::uint32_t raw) noexcept;
bool is_valid_review_state(std::uint32_t raw) noexcept;
bool is_valid_review_disposition(std::uint32_t raw) noexcept;
bool is_valid_aggregation_policy(std::uint32_t raw) noexcept;
bool is_valid_disagreement_policy(std::uint32_t raw) noexcept;
bool is_valid_duplicate_policy(std::uint32_t raw) noexcept;
bool is_valid_challenge_policy(std::uint32_t raw) noexcept;
bool is_valid_challenge_outcome(std::uint32_t raw) noexcept;
bool is_valid_predicate_kind(std::uint32_t raw) noexcept;
bool is_valid_confidence_basis(std::uint32_t raw) noexcept;

// Severity ordinal comparison helper. Never used to establish truth.
constexpr std::uint32_t severity_rank(Severity value) noexcept {
    return static_cast<std::uint32_t>(value);
}

// True when the outcome is a decisive assertion about the target. Abstain,
// Unknown, NotApplicable and Inconclusive are explicitly not decisive.
constexpr bool is_decisive_outcome(FindingOutcome outcome) noexcept {
    return outcome == FindingOutcome::Pass || outcome == FindingOutcome::Fail ||
           outcome == FindingOutcome::Warn;
}

// True when the outcome carries no assertion at all.
constexpr bool is_non_asserting_outcome(FindingOutcome outcome) noexcept {
    return outcome == FindingOutcome::Abstain;
}

// True when the outcome is exactly UNKNOWN. UNKNOWN must never silently become
// PASS, VERIFIED, SAFE, CURRENT, ELIGIBLE, AUTHORITATIVE, CONSENSUS or
// SUFFICIENT_EVIDENCE.
constexpr bool is_unknown_outcome(FindingOutcome outcome) noexcept {
    return outcome == FindingOutcome::Unknown;
}

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_ENUMS_HPP

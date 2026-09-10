// Critic Fabric — enumeration names and exact parsing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/enums.hpp"

#include <cstddef>

namespace critic_fabric {
namespace {

struct EnumEntry {
    std::uint32_t value;
    const char* name;
};

template <std::size_t N>
const char* lookup(const EnumEntry (&table)[N], std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (table[i].value == value) {
            return table[i].name;
        }
    }
    return "Invalid";
}

template <std::size_t N>
bool parse(const EnumEntry (&table)[N], std::string_view text, std::uint32_t& out) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (text == table[i].name) {
            out = table[i].value;
            return true;
        }
    }
    return false;
}

template <std::size_t N>
bool valid(const EnumEntry (&table)[N], std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (table[i].value == value) {
            return true;
        }
    }
    return false;
}

constexpr EnumEntry kTargetClass[] = {
    {1, "CLAIM"},           {2, "ARTIFACT"},        {3, "PLAN"},
    {4, "RESULT"},          {5, "MODEL_OUTPUT"},    {6, "EXECUTION_OUTPUT"},
    {7, "CONFIGURATION"},   {8, "POLICY_RESULT"},   {9, "DATASET_SLICE"},
    {10, "OTHER_TYPED_TARGET"}};

constexpr EnumEntry kCriticRole[] = {
    {1, "GENERAL_CRITIC"},        {2, "FACT_CHECKER"},        {3, "LOGIC_CRITIC"},
    {4, "SAFETY_CRITIC"},         {5, "PERFORMANCE_CRITIC"},  {6, "SECURITY_CRITIC"},
    {7, "CORRECTNESS_VERIFIER"},  {8, "CONSISTENCY_VERIFIER"},{9, "SCHEMA_VERIFIER"},
    {10, "ADVERSARIAL_CRITIC"},   {11, "DOMAIN_SPECIALIST"},  {12, "REBUTTAL_CRITIC"}};

constexpr EnumEntry kFindingCategory[] = {
    {1, "CORRECTNESS"},   {2, "CONSISTENCY"},     {3, "SCHEMA"},   {4, "SAFETY"},
    {5, "SECURITY"},      {6, "PERFORMANCE"},     {7, "LOGIC"},    {8, "FACTUAL"},
    {9, "EVIDENCE_QUALITY"}, {10, "POLICY"},      {11, "OTHER"}};

constexpr EnumEntry kSeverity[] = {
    {1, "INFO"}, {2, "LOW"}, {3, "MEDIUM"}, {4, "HIGH"}, {5, "CRITICAL"}};

constexpr EnumEntry kFindingOutcome[] = {
    {1, "PASS"},     {2, "FAIL"},        {3, "WARN"},          {4, "ABSTAIN"},
    {5, "UNKNOWN"},  {6, "NOT_APPLICABLE"}, {7, "INCONCLUSIVE"}};

constexpr EnumEntry kEvidenceSourceType[] = {
    {1, "INSTRUMENTATION"}, {2, "TEST_RUN"},      {3, "STATIC_ANALYSIS"},
    {4, "EXECUTION_TRACE"}, {5, "EXTERNAL_REPORT"},{6, "COMPUTATION"},
    {7, "HUMAN_ASSERTION"}, {8, "OTHER"}};

constexpr EnumEntry kEvidenceProvenance[] = {
    {1, "MEASURED"},   {2, "OBSERVED"},     {3, "DERIVED"},      {4, "REPORTED"},
    {5, "SYNTHETIC"},  {6, "RECONSTRUCTED"},{7, "UNKNOWN"}};

constexpr EnumEntry kEvidenceIntegrity[] = {
    {1, "VERIFIED"}, {2, "UNVERIFIED"}, {3, "FAILED"}, {4, "UNKNOWN"}};

constexpr EnumEntry kFindingState[] = {
    {1, "CURRENT"},     {2, "SUPERSEDED"}, {3, "INVALIDATED"}, {4, "CHALLENGED"},
    {5, "UPHELD"},      {6, "REBUTTED"},   {7, "WITHDRAWN"}};

constexpr EnumEntry kTargetSupersession[] = {{1, "CURRENT"}, {2, "SUPERSEDED"}, {3, "RETIRED"}};

constexpr EnumEntry kReviewState[] = {
    {1, "DECLARED"},      {2, "ASSIGNING"},           {3, "ASSIGNED"},
    {4, "REVIEWING"},     {5, "FINDINGS_RECEIVED"},   {6, "VALIDATING"},
    {7, "CHALLENGED"},    {8, "REBUTTAL_PENDING"},    {9, "EVALUATING"},
    {10, "RESOLVED"},     {11, "COMMITTED"},          {12, "REVALIDATION_REQUIRED"},
    {13, "CANCELLED"},    {14, "SUPERSEDED"},         {15, "FAILED"},
    {16, "RETIRED"}};

constexpr EnumEntry kReviewDisposition[] = {
    {0, "NONE"},                  {1, "PASS"},                   {2, "FAIL"},
    {3, "WARN"},                  {4, "DISAGREEMENT"},           {5, "TIE"},
    {6, "INSUFFICIENT_EVIDENCE"}, {7, "REQUIRED_CRITIC_FAILED"}, {8, "QUORUM_NOT_REACHED"},
    {9, "REVALIDATION_REQUIRED"}, {10, "CANCELLED"},             {11, "SUPERSEDED"},
    {12, "UNKNOWN"}};

constexpr EnumEntry kAggregationPolicy[] = {
    {1, "ALL_REQUIRED_PASS"},        {2, "ANY_CRITICAL_FAIL"},
    {3, "QUORUM_PASS"},              {4, "WEIGHTED_REVIEW"},
    {5, "REQUIRED_ROLE_PASS"},       {6, "MAJORITY_WITH_CONSTRAINTS"},
    {7, "CONSENSUS_THRESHOLD"},      {8, "POLICY_TABLE"}};

constexpr EnumEntry kDisagreementPolicy[] = {
    {1, "REPORT_AND_FAIL"}, {2, "REPORT_AND_WARN"}, {3, "THRESHOLD_THEN_FAIL"}, {4, "ESCALATE"}};

constexpr EnumEntry kDuplicatePolicy[] = {
    {1, "REJECT"}, {2, "IDEMPOTENT_IGNORE"}, {3, "KEEP_DISTINCT"}};

constexpr EnumEntry kChallengePolicy[] = {
    {1, "DISABLED"}, {2, "ALLOW_ONE_ROUND"}, {3, "ALLOW_BOUNDED_ROUNDS"}};

constexpr EnumEntry kChallengeOutcome[] = {
    {1, "UNRESOLVED"}, {2, "UPHELD"}, {3, "MODIFIED"}, {4, "SUPERSEDED"}, {5, "INVALIDATED"}};

constexpr EnumEntry kPredicateKind[] = {
    {1, "EXACT_TARGET_GENERATION_MATCH"},     {2, "TARGET_DIGEST_MATCH"},
    {3, "REQUIRED_EVIDENCE_PRESENT"},         {4, "EVIDENCE_INTEGRITY_VERIFIED"},
    {5, "EVIDENCE_PROVENANCE_KNOWN"},         {6, "REQUIRED_ROLE_PRESENT"},
    {7, "MINIMUM_INDEPENDENT_CRITIC_IDENTITIES"},
    {8, "REQUIRED_VERIFIER_PASS"},            {9, "NO_CRITICAL_FAIL_FINDINGS"},
    {10, "CRITIC_GENERATION_CURRENT"},        {11, "WORKER_BOOT_CURRENT"},
    {12, "COORDINATOR_EPOCH_CURRENT"},        {13, "REQUIRED_CATEGORIES_COVERED"},
    {14, "QUORUM_SATISFIED"},                 {15, "CONFIDENCE_NOT_AUTHORITATIVE"}};

constexpr EnumEntry kConfidenceBasis[] = {
    {0, "NOT_PROVIDED"}, {1, "DECLARED_BY_CRITIC"}, {2, "DERIVED_FROM_EVIDENCE_COUNT"}};

}  // namespace

#define CRITIC_FABRIC_ENUM_NAME_FN(fn, type, table)      \
    const char* fn(type value) noexcept {                \
        return lookup(table, static_cast<std::uint32_t>(value)); \
    }

CRITIC_FABRIC_ENUM_NAME_FN(to_string, TargetClass, kTargetClass)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, CriticRole, kCriticRole)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, FindingCategory, kFindingCategory)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, Severity, kSeverity)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, FindingOutcome, kFindingOutcome)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, EvidenceSourceType, kEvidenceSourceType)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, EvidenceProvenance, kEvidenceProvenance)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, EvidenceIntegrity, kEvidenceIntegrity)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, FindingState, kFindingState)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, TargetSupersession, kTargetSupersession)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, ReviewState, kReviewState)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, ReviewDisposition, kReviewDisposition)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, AggregationPolicy, kAggregationPolicy)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, DisagreementPolicy, kDisagreementPolicy)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, DuplicatePolicy, kDuplicatePolicy)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, ChallengePolicy, kChallengePolicy)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, ChallengeOutcome, kChallengeOutcome)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, PredicateKind, kPredicateKind)
CRITIC_FABRIC_ENUM_NAME_FN(to_string, ConfidenceBasis, kConfidenceBasis)

#undef CRITIC_FABRIC_ENUM_NAME_FN

#define CRITIC_FABRIC_ENUM_PARSE_FN(fn, type, table)                              \
    bool fn(std::string_view text, type& out) noexcept {                          \
        std::uint32_t raw = 0;                                                    \
        if (!parse(table, text, raw)) {                                           \
            return false;                                                         \
        }                                                                         \
        out = static_cast<type>(raw);                                             \
        return true;                                                              \
    }

CRITIC_FABRIC_ENUM_PARSE_FN(parse_target_class, TargetClass, kTargetClass)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_critic_role, CriticRole, kCriticRole)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_finding_category, FindingCategory, kFindingCategory)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_severity, Severity, kSeverity)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_finding_outcome, FindingOutcome, kFindingOutcome)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_evidence_provenance, EvidenceProvenance, kEvidenceProvenance)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_evidence_source_type, EvidenceSourceType, kEvidenceSourceType)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_review_state, ReviewState, kReviewState)
CRITIC_FABRIC_ENUM_PARSE_FN(parse_review_disposition, ReviewDisposition, kReviewDisposition)

#undef CRITIC_FABRIC_ENUM_PARSE_FN

#define CRITIC_FABRIC_ENUM_VALID_FN(fn, table)         \
    bool fn(std::uint32_t raw) noexcept { return valid(table, raw); }

CRITIC_FABRIC_ENUM_VALID_FN(is_valid_target_class, kTargetClass)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_critic_role, kCriticRole)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_finding_category, kFindingCategory)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_severity, kSeverity)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_finding_outcome, kFindingOutcome)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_evidence_source_type, kEvidenceSourceType)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_evidence_provenance, kEvidenceProvenance)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_evidence_integrity, kEvidenceIntegrity)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_finding_state, kFindingState)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_target_supersession, kTargetSupersession)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_review_state, kReviewState)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_review_disposition, kReviewDisposition)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_aggregation_policy, kAggregationPolicy)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_disagreement_policy, kDisagreementPolicy)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_duplicate_policy, kDuplicatePolicy)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_challenge_policy, kChallengePolicy)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_challenge_outcome, kChallengeOutcome)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_predicate_kind, kPredicateKind)
CRITIC_FABRIC_ENUM_VALID_FN(is_valid_confidence_basis, kConfidenceBasis)

#undef CRITIC_FABRIC_ENUM_VALID_FN

}  // namespace critic_fabric

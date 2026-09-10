// Critic Fabric — typed outcome rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/result.hpp"

namespace critic_fabric {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::StaleReviewGeneration: return "StaleReviewGeneration";
        case ErrorCode::StaleTargetGeneration: return "StaleTargetGeneration";
        case ErrorCode::StaleCriticGeneration: return "StaleCriticGeneration";
        case ErrorCode::StaleWorkerBoot: return "StaleWorkerBoot";
        case ErrorCode::StaleCoordinatorEpoch: return "StaleCoordinatorEpoch";
        case ErrorCode::CriticNotAuthoritative: return "CriticNotAuthoritative";
        case ErrorCode::CriticNotRegistered: return "CriticNotRegistered";
        case ErrorCode::CriticCapabilityMismatch: return "CriticCapabilityMismatch";
        case ErrorCode::WorkerNotRegistered: return "WorkerNotRegistered";
        case ErrorCode::DuplicateCriticIdentity: return "DuplicateCriticIdentity";
        case ErrorCode::DuplicateWorkerIdentity: return "DuplicateWorkerIdentity";
        case ErrorCode::CriticGenerationRegressed: return "CriticGenerationRegressed";
        case ErrorCode::InvalidIdentity: return "InvalidIdentity";
        case ErrorCode::TargetUnknown: return "TargetUnknown";
        case ErrorCode::TargetNotReviewable: return "TargetNotReviewable";
        case ErrorCode::TargetMismatch: return "TargetMismatch";
        case ErrorCode::TargetGenerationRegressed: return "TargetGenerationRegressed";
        case ErrorCode::TargetSuperseded: return "TargetSuperseded";
        case ErrorCode::TargetDigestMismatch: return "TargetDigestMismatch";
        case ErrorCode::ReviewUnknown: return "ReviewUnknown";
        case ErrorCode::ReviewAlreadyExists: return "ReviewAlreadyExists";
        case ErrorCode::ReviewCancelled: return "ReviewCancelled";
        case ErrorCode::ReviewSuperseded: return "ReviewSuperseded";
        case ErrorCode::ReviewNotCommittable: return "ReviewNotCommittable";
        case ErrorCode::InvalidStateTransition: return "InvalidStateTransition";
        case ErrorCode::ReviewNotOpen: return "ReviewNotOpen";
        case ErrorCode::ReviewClosed: return "ReviewClosed";
        case ErrorCode::FindingUnknown: return "FindingUnknown";
        case ErrorCode::MalformedFinding: return "MalformedFinding";
        case ErrorCode::MalformedEvidence: return "MalformedEvidence";
        case ErrorCode::DuplicateFinding: return "DuplicateFinding";
        case ErrorCode::DuplicateEvidence: return "DuplicateEvidence";
        case ErrorCode::ConflictingFinding: return "ConflictingFinding";
        case ErrorCode::WrongTargetFinding: return "WrongTargetFinding";
        case ErrorCode::EvidenceNotCurrent: return "EvidenceNotCurrent";
        case ErrorCode::EvidenceIntegrityFailed: return "EvidenceIntegrityFailed";
        case ErrorCode::EvidenceDigestMismatch: return "EvidenceDigestMismatch";
        case ErrorCode::EvidenceProvenanceUnknown: return "EvidenceProvenanceUnknown";
        case ErrorCode::FindingSuperseded: return "FindingSuperseded";
        case ErrorCode::InsufficientEvidence: return "InsufficientEvidence";
        case ErrorCode::RequiredCriticUnavailable: return "RequiredCriticUnavailable";
        case ErrorCode::RequiredCriticFailed: return "RequiredCriticFailed";
        case ErrorCode::QuorumNotReached: return "QuorumNotReached";
        case ErrorCode::DisagreementUnresolved: return "DisagreementUnresolved";
        case ErrorCode::HardPredicateFailed: return "HardPredicateFailed";
        case ErrorCode::PolicyRejected: return "PolicyRejected";
        case ErrorCode::ResultAlreadyCommitted: return "ResultAlreadyCommitted";
        case ErrorCode::ConflictingReviewCommit: return "ConflictingReviewCommit";
        case ErrorCode::DecisionUnknown: return "DecisionUnknown";
        case ErrorCode::StaleDecisionGeneration: return "StaleDecisionGeneration";
        case ErrorCode::RevalidationRequired: return "RevalidationRequired";
        case ErrorCode::ChallengeUnknown: return "ChallengeUnknown";
        case ErrorCode::InvalidChallenge: return "InvalidChallenge";
        case ErrorCode::StaleChallenge: return "StaleChallenge";
        case ErrorCode::InvalidRebuttal: return "InvalidRebuttal";
        case ErrorCode::StaleRebuttal: return "StaleRebuttal";
        case ErrorCode::ChallengeLimitExceeded: return "ChallengeLimitExceeded";
        case ErrorCode::ChallengeDisabled: return "ChallengeDisabled";
        case ErrorCode::PersistenceCorruption: return "PersistenceCorruption";
        case ErrorCode::PersistenceTruncated: return "PersistenceTruncated";
        case ErrorCode::PersistenceTrailingData: return "PersistenceTrailingData";
        case ErrorCode::PersistenceUnsupportedVersion: return "PersistenceUnsupportedVersion";
        case ErrorCode::PersistenceBadMagic: return "PersistenceBadMagic";
        case ErrorCode::PersistenceIoError: return "PersistenceIoError";
        case ErrorCode::PersistenceChecksumMismatch: return "PersistenceChecksumMismatch";
        case ErrorCode::PersistenceUnavailable: return "PersistenceUnavailable";
        case ErrorCode::ProtocolError: return "ProtocolError";
        case ErrorCode::ProtocolBadMagic: return "ProtocolBadMagic";
        case ErrorCode::ProtocolBadVersion: return "ProtocolBadVersion";
        case ErrorCode::ProtocolBadChecksum: return "ProtocolBadChecksum";
        case ErrorCode::ProtocolTruncated: return "ProtocolTruncated";
        case ErrorCode::ProtocolOversized: return "ProtocolOversized";
        case ErrorCode::ProtocolTrailingGarbage: return "ProtocolTrailingGarbage";
        case ErrorCode::ProtocolInvalidEnum: return "ProtocolInvalidEnum";
        case ErrorCode::ProtocolUnknownMessageType: return "ProtocolUnknownMessageType";
        case ErrorCode::ProtocolConnectionClosed: return "ProtocolConnectionClosed";
        case ErrorCode::ProtocolTimeout: return "ProtocolTimeout";
        case ErrorCode::ResourceLimitExceeded: return "ResourceLimitExceeded";
        case ErrorCode::ArithmeticOverflow: return "ArithmeticOverflow";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::InternalError: return "InternalError";
    }
    return "UnknownErrorCode";
}

bool is_retryable_fence(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::StaleReviewGeneration:
        case ErrorCode::StaleTargetGeneration:
        case ErrorCode::StaleCriticGeneration:
        case ErrorCode::StaleWorkerBoot:
        case ErrorCode::StaleCoordinatorEpoch:
        case ErrorCode::CriticNotAuthoritative:
        case ErrorCode::WorkerNotRegistered:
        case ErrorCode::RevalidationRequired:
            return true;
        default:
            return false;
    }
}

std::string Status::to_string() const {
    std::string out = critic_fabric::to_string(code_);
    if (!detail_.empty()) {
        out += ": ";
        out += detail_;
    }
    return out;
}

}  // namespace critic_fabric

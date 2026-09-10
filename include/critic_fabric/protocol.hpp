// Critic Fabric — framed binary transport protocol.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_PROTOCOL_HPP
#define CRITIC_FABRIC_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "critic_fabric/authority.hpp"
#include "critic_fabric/challenge.hpp"
#include "critic_fabric/critic.hpp"
#include "critic_fabric/evidence.hpp"
#include "critic_fabric/explanation.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/limits.hpp"
#include "critic_fabric/result.hpp"
#include "critic_fabric/review.hpp"
#include "critic_fabric/serialization.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

// Numeric values are part of the wire format and must never be renumbered.
enum class MessageType : std::uint16_t {
    Invalid = 0,
    Hello = 1,
    HelloAck = 2,
    RegisterTarget = 10,
    RegisterTargetAck = 11,
    SupersedeTarget = 12,
    SupersedeTargetAck = 13,
    RegisterCritic = 20,
    RegisterCriticAck = 21,
    RegisterWorker = 22,
    RegisterWorkerAck = 23,
    DeclareReadiness = 24,
    DeclareReadinessAck = 25,
    DeclareReview = 30,
    DeclareReviewAck = 31,
    AssignCritics = 32,
    AssignCriticsAck = 33,
    SubmitEvidence = 40,
    SubmitEvidenceAck = 41,
    SubmitFinding = 50,
    SubmitFindingAck = 51,
    SubmitChallenge = 52,
    SubmitChallengeAck = 53,
    SubmitRebuttal = 54,
    SubmitRebuttalAck = 55,
    CommitReview = 60,
    CommitReviewAck = 61,
    CancelReview = 62,
    CancelReviewAck = 63,
    QueryReview = 70,
    QueryReviewAck = 71,
    QueryDecision = 72,
    QueryDecisionAck = 73,
    ExplainReview = 74,
    ExplainReviewAck = 75,
    ListReviews = 76,
    ListReviewsAck = 77,
    QueryAssignments = 78,
    QueryAssignmentsAck = 79,
    AssignmentNotice = 80,
    FenceWorker = 82,
    FenceWorkerAck = 83,
    QueryWorkers = 84,
    QueryWorkersAck = 85,
    QueryTarget = 86,
    QueryTargetAck = 87,
    ErrorResponse = 90,
    Shutdown = 91,
};
inline constexpr std::uint16_t kMessageTypeMax = 91;

const char* to_string(MessageType type) noexcept;
bool is_valid_message_type(std::uint16_t raw) noexcept;

// Fixed frame header. Layout (big-endian, 108 bytes):
//   magic[4] 'C','F','B','1' | version u16 | type u16 | flags u32 |
//   payload_length u32 | correlation RequestId u64 | authority 80 bytes |
//   checksum u32 (CRC-32 over bytes [0,104) followed by the payload)
struct FrameHeader {
    std::uint16_t version = 1;
    MessageType type = MessageType::Invalid;
    std::uint32_t flags = 0;
    std::uint32_t payload_length = 0;
    RequestId correlation{};
    AuthorityContext authority{};
    std::uint32_t checksum = 0;
};

struct Frame {
    FrameHeader header;
    std::vector<std::uint8_t> payload;
};

class FrameCodec {
public:
    static constexpr std::size_t kHeaderBytes = 108;
    // Bytes of the header that precede the checksum field.
    static constexpr std::size_t kChecksumCoverageBytes = 104;
    static constexpr std::size_t kMagicBytes = 4;
    static constexpr std::uint16_t kProtocolVersion = 1;
    static constexpr std::uint32_t kMaximumFlags = 0;

    static Status encode(const Frame& frame, const RuntimeLimits& limits, std::vector<std::uint8_t>& out);
    static Status decode_header(const std::uint8_t* data, std::size_t len, const RuntimeLimits& limits,
                                FrameHeader& out);
    static Status decode_payload(const FrameHeader& header, const std::uint8_t* data, std::size_t len,
                                 const RuntimeLimits& limits, std::vector<std::uint8_t>& out);
    // Whole-buffer decode. The buffer must contain exactly one frame; trailing
    // bytes are reported as ProtocolTrailingGarbage rather than ignored.
    static Status decode(const std::vector<std::uint8_t>& buffer, const RuntimeLimits& limits, Frame& out);
};

// --- Payload codecs ----------------------------------------------------------

struct StatusEnvelope {
    ErrorCode code = ErrorCode::Ok;
    std::string detail;
};

void encode(ByteWriter& writer, const StatusEnvelope& value);
Status decode(ByteReader& reader, StatusEnvelope& out, const RuntimeLimits& limits);

void encode(ByteWriter& writer, const RegisterTargetRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, RegisterTargetRequest& out);
void encode(ByteWriter& writer, const SupersedeTargetRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, SupersedeTargetRequest& out);

void encode(ByteWriter& writer, const RegisterCriticRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, RegisterCriticRequest& out);
void encode(ByteWriter& writer, const RegisterWorkerRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, RegisterWorkerRequest& out);
void encode(ByteWriter& writer, const DeclareReadinessRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, DeclareReadinessRequest& out);

void encode(ByteWriter& writer, const DeclareReviewRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, DeclareReviewRequest& out);
void encode(ByteWriter& writer, const AssignCriticsRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, AssignCriticsRequest& out);

void encode(ByteWriter& writer, const SubmitEvidenceRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitEvidenceRequest& out);
void encode(ByteWriter& writer, const SubmitFindingRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitFindingRequest& out);
void encode(ByteWriter& writer, const SubmitChallengeRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitChallengeRequest& out);
void encode(ByteWriter& writer, const SubmitRebuttalRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitRebuttalRequest& out);

void encode(ByteWriter& writer, const CommitReviewRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, CommitReviewRequest& out);
void encode(ByteWriter& writer, const CancelReviewRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, CancelReviewRequest& out);

void encode(ByteWriter& writer, const ReviewSnapshot& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, ReviewSnapshot& out);
void encode(ByteWriter& writer, const ReviewSummary& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, ReviewSummary& out);
void encode(ByteWriter& writer, const CriticAssignment& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, CriticAssignment& out);
void encode(ByteWriter& writer, const WorkerRecord& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, WorkerRecord& out);
void encode(ByteWriter& writer, const Decision& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, Decision& out);
void encode(ByteWriter& writer, const Finding& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, Finding& out);
void encode(ByteWriter& writer, const TargetRecord& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, TargetRecord& out);
void encode(ByteWriter& writer, const Challenge& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, Challenge& out);
void encode(ByteWriter& writer, const Rebuttal& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, Rebuttal& out);
void encode(ByteWriter& writer, const EvidenceRecord& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, EvidenceRecord& out);
void encode(ByteWriter& writer, const CriticRegistration& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, CriticRegistration& out);
void encode(ByteWriter& writer, const ReviewRecord& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, ReviewRecord& out);
// Fence requests name one worker incarnation explicitly.
struct FenceWorkerRequest {
    WorkerId worker{};
    std::string reason;
    CoordinatorEpoch epoch{};
    RequestId request{};
};
void encode(ByteWriter& writer, const FenceWorkerRequest& value, const RuntimeLimits& limits);
Status decode(ByteReader& reader, const RuntimeLimits& limits, FenceWorkerRequest& out);

// Response body helpers: a status envelope followed by a typed value.
template <typename T, typename EncodeFn>
void encode_typed_response(ByteWriter& writer, const StatusEnvelope& envelope, const T& value,
                           const RuntimeLimits& limits, EncodeFn encode_fn) {
    encode(writer, envelope);
    encode_fn(writer, value, limits);
}

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_PROTOCOL_HPP

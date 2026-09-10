// Critic Fabric — framed binary transport protocol implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/protocol.hpp"

#include <cstring>

namespace critic_fabric {
namespace {

constexpr std::uint8_t kMagic[FrameCodec::kMagicBytes] = {'C', 'F', 'B', '1'};

ReaderPolicy protocol_reader_policy() {
    ReaderPolicy policy;
    policy.truncated = ErrorCode::ProtocolTruncated;
    policy.oversized = ErrorCode::ProtocolOversized;
    policy.invalid_enum = ErrorCode::ProtocolInvalidEnum;
    policy.malformed = ErrorCode::ProtocolError;
    policy.overflow = ErrorCode::ArithmeticOverflow;
    return policy;
}

void write_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

std::uint32_t read_u32_be(const std::uint8_t* p) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<std::uint32_t>(p[i]);
    }
    return value;
}

void encode_id_list(ByteWriter& writer, const std::vector<EvidenceId>& ids, std::uint32_t max_items) {
    if (ids.size() > max_items) {
        writer.fail(ErrorCode::ResourceLimitExceeded, "evidence reference list exceeds the configured bound");
        return;
    }
    writer.u32(static_cast<std::uint32_t>(ids.size()));
    for (EvidenceId id : ids) {
        writer.u64(id.value());
    }
}

Status decode_id_list(ByteReader& reader, std::vector<EvidenceId>& out, std::uint32_t max_items) {
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > max_items) {
        return Status::error(ErrorCode::ProtocolOversized, "evidence reference list exceeds the configured bound");
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(EvidenceId::from_value(reader.u64()));
    }
    return reader.status();
}

void encode_authority_fields(ByteWriter& writer, const AuthorityContext& authority) {
    writer.u64(authority.epoch.value());
    writer.u64(authority.worker.value());
    writer.u64(authority.worker_boot.value());
    writer.u64(authority.critic.value());
    writer.u64(authority.critic_generation.value());
    writer.u64(authority.review.value());
    writer.u64(authority.review_generation.value());
    writer.u64(authority.target.value());
    writer.u64(authority.target_generation.value());
    writer.u64(authority.request.value());
}

}  // namespace

const char* to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::Invalid: return "INVALID";
        case MessageType::Hello: return "HELLO";
        case MessageType::HelloAck: return "HELLO_ACK";
        case MessageType::RegisterTarget: return "REGISTER_TARGET";
        case MessageType::RegisterTargetAck: return "REGISTER_TARGET_ACK";
        case MessageType::SupersedeTarget: return "SUPERSEDE_TARGET";
        case MessageType::SupersedeTargetAck: return "SUPERSEDE_TARGET_ACK";
        case MessageType::RegisterCritic: return "REGISTER_CRITIC";
        case MessageType::RegisterCriticAck: return "REGISTER_CRITIC_ACK";
        case MessageType::RegisterWorker: return "REGISTER_WORKER";
        case MessageType::RegisterWorkerAck: return "REGISTER_WORKER_ACK";
        case MessageType::DeclareReadiness: return "DECLARE_READINESS";
        case MessageType::DeclareReadinessAck: return "DECLARE_READINESS_ACK";
        case MessageType::DeclareReview: return "DECLARE_REVIEW";
        case MessageType::DeclareReviewAck: return "DECLARE_REVIEW_ACK";
        case MessageType::AssignCritics: return "ASSIGN_CRITICS";
        case MessageType::AssignCriticsAck: return "ASSIGN_CRITICS_ACK";
        case MessageType::SubmitEvidence: return "SUBMIT_EVIDENCE";
        case MessageType::SubmitEvidenceAck: return "SUBMIT_EVIDENCE_ACK";
        case MessageType::SubmitFinding: return "SUBMIT_FINDING";
        case MessageType::SubmitFindingAck: return "SUBMIT_FINDING_ACK";
        case MessageType::SubmitChallenge: return "SUBMIT_CHALLENGE";
        case MessageType::SubmitChallengeAck: return "SUBMIT_CHALLENGE_ACK";
        case MessageType::SubmitRebuttal: return "SUBMIT_REBUTTAL";
        case MessageType::SubmitRebuttalAck: return "SUBMIT_REBUTTAL_ACK";
        case MessageType::CommitReview: return "COMMIT_REVIEW";
        case MessageType::CommitReviewAck: return "COMMIT_REVIEW_ACK";
        case MessageType::CancelReview: return "CANCEL_REVIEW";
        case MessageType::CancelReviewAck: return "CANCEL_REVIEW_ACK";
        case MessageType::QueryReview: return "QUERY_REVIEW";
        case MessageType::QueryReviewAck: return "QUERY_REVIEW_ACK";
        case MessageType::QueryDecision: return "QUERY_DECISION";
        case MessageType::QueryDecisionAck: return "QUERY_DECISION_ACK";
        case MessageType::ExplainReview: return "EXPLAIN_REVIEW";
        case MessageType::ExplainReviewAck: return "EXPLAIN_REVIEW_ACK";
        case MessageType::ListReviews: return "LIST_REVIEWS";
        case MessageType::ListReviewsAck: return "LIST_REVIEWS_ACK";

        case MessageType::QueryAssignments: return "QUERY_ASSIGNMENTS";
        case MessageType::QueryAssignmentsAck: return "QUERY_ASSIGNMENTS_ACK";
        case MessageType::AssignmentNotice: return "ASSIGNMENT_NOTICE";
        case MessageType::FenceWorker: return "FENCE_WORKER";
        case MessageType::FenceWorkerAck: return "FENCE_WORKER_ACK";
        case MessageType::QueryWorkers: return "QUERY_WORKERS";
        case MessageType::QueryWorkersAck: return "QUERY_WORKERS_ACK";
        case MessageType::QueryTarget: return "QUERY_TARGET";
        case MessageType::QueryTargetAck: return "QUERY_TARGET_ACK";
        case MessageType::ErrorResponse: return "ERROR_RESPONSE";
        case MessageType::Shutdown: return "SHUTDOWN";
    }
    return "UNKNOWN";
}

bool is_valid_message_type(std::uint16_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::RegisterTarget:
        case MessageType::RegisterTargetAck:
        case MessageType::SupersedeTarget:
        case MessageType::SupersedeTargetAck:
        case MessageType::RegisterCritic:
        case MessageType::RegisterCriticAck:
        case MessageType::RegisterWorker:
        case MessageType::RegisterWorkerAck:
        case MessageType::DeclareReadiness:
        case MessageType::DeclareReadinessAck:
        case MessageType::DeclareReview:
        case MessageType::DeclareReviewAck:
        case MessageType::AssignCritics:
        case MessageType::AssignCriticsAck:
        case MessageType::SubmitEvidence:
        case MessageType::SubmitEvidenceAck:
        case MessageType::SubmitFinding:
        case MessageType::SubmitFindingAck:
        case MessageType::SubmitChallenge:
        case MessageType::SubmitChallengeAck:
        case MessageType::SubmitRebuttal:
        case MessageType::SubmitRebuttalAck:
        case MessageType::CommitReview:
        case MessageType::CommitReviewAck:
        case MessageType::CancelReview:
        case MessageType::CancelReviewAck:
        case MessageType::QueryReview:
        case MessageType::QueryReviewAck:
        case MessageType::QueryDecision:
        case MessageType::QueryDecisionAck:
        case MessageType::ExplainReview:
        case MessageType::ExplainReviewAck:
        case MessageType::ListReviews:
        case MessageType::ListReviewsAck:
        case MessageType::QueryAssignments:
        case MessageType::QueryAssignmentsAck:
        case MessageType::AssignmentNotice:
        case MessageType::FenceWorker:
        case MessageType::FenceWorkerAck:
        case MessageType::QueryWorkers:
        case MessageType::QueryWorkersAck:
        case MessageType::QueryTarget:
        case MessageType::QueryTargetAck:
        case MessageType::ErrorResponse:
        case MessageType::Shutdown:
            return true;
        case MessageType::Invalid:
            return false;
    }
    return false;
}

Status FrameCodec::encode(const Frame& frame, const RuntimeLimits& limits, std::vector<std::uint8_t>& out) {
    if (!is_valid_message_type(static_cast<std::uint16_t>(frame.header.type))) {
        return Status::error(ErrorCode::ProtocolUnknownMessageType,
                             std::string("cannot encode an unknown message type: ") +
                                 std::to_string(static_cast<std::uint16_t>(frame.header.type)));
    }
    if (frame.header.version != kProtocolVersion) {
        return Status::error(ErrorCode::ProtocolBadVersion, "cannot encode a foreign protocol version");
    }
    if (frame.header.flags > kMaximumFlags) {
        return Status::error(ErrorCode::ProtocolError, "cannot encode reserved frame flags");
    }
    if (frame.payload.size() > limits.max_frame_bytes) {
        return Status::error(ErrorCode::ProtocolOversized, "frame payload exceeds the configured frame bound");
    }

    out.clear();
    out.reserve(kHeaderBytes + frame.payload.size());
    out.insert(out.end(), kMagic, kMagic + kMagicBytes);
    out.push_back(static_cast<std::uint8_t>((frame.header.version >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(frame.header.version & 0xFFu));
    const std::uint16_t type_raw = static_cast<std::uint16_t>(frame.header.type);
    out.push_back(static_cast<std::uint8_t>((type_raw >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(type_raw & 0xFFu));
    write_u32_be(out, frame.header.flags);
    write_u32_be(out, static_cast<std::uint32_t>(frame.payload.size()));
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((frame.header.correlation.value() >> shift) & 0xFFu));
    }
    ByteWriter authority_writer;
    encode_authority_fields(authority_writer, frame.header.authority);
    if (!authority_writer.ok() || authority_writer.size() != 80) {
        return Status::error(ErrorCode::InternalError, "authority block did not encode to its fixed size");
    }
    out.insert(out.end(), authority_writer.buffer().begin(), authority_writer.buffer().end());

    // The checksum covers everything written so far plus the payload, so a
    // corrupted length field or authority block is detected as reliably as a
    // corrupted payload.
    const std::uint32_t header_crc = crc32(out.data(), out.size());
    const std::uint32_t full_crc = crc32_append(header_crc, frame.payload);
    write_u32_be(out, full_crc);
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return Status::success();
}

Status FrameCodec::decode_header(const std::uint8_t* data, std::size_t len, const RuntimeLimits& limits,
                                 FrameHeader& out) {
    if (data == nullptr) {
        return Status::error(ErrorCode::ProtocolError, "null frame buffer");
    }
    if (len < kHeaderBytes) {
        return Status::error(ErrorCode::ProtocolTruncated, "frame header is incomplete");
    }
    if (std::memcmp(data, kMagic, kMagicBytes) != 0) {
        return Status::error(ErrorCode::ProtocolBadMagic, "frame magic does not match");
    }
    out.version = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[4]) << 8) | data[5]);
    if (out.version != kProtocolVersion) {
        return Status::error(ErrorCode::ProtocolBadVersion, "frame protocol version is not supported");
    }
    const std::uint16_t type_raw = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[6]) << 8) | data[7]);
    if (!is_valid_message_type(type_raw)) {
        return Status::error(ErrorCode::ProtocolUnknownMessageType, "frame carries an unknown message type");
    }
    out.type = static_cast<MessageType>(type_raw);
    out.flags = read_u32_be(data + 8);
    if (out.flags > kMaximumFlags) {
        return Status::error(ErrorCode::ProtocolError, "frame carries reserved flags");
    }
    out.payload_length = read_u32_be(data + 12);
    if (out.payload_length > limits.max_frame_bytes) {
        return Status::error(ErrorCode::ProtocolOversized, "declared frame payload exceeds the frame bound");
    }
    std::uint64_t correlation = 0;
    for (int i = 0; i < 8; ++i) {
        correlation = (correlation << 8) | static_cast<std::uint64_t>(data[16 + i]);
    }
    out.correlation = RequestId::from_value(correlation);
    ByteReader authority_reader(data + 24, 80, protocol_reader_policy());
    out.authority.epoch = CoordinatorEpoch::from_value(authority_reader.u64());
    out.authority.worker = WorkerId::from_value(authority_reader.u64());
    out.authority.worker_boot = WorkerBootId::from_value(authority_reader.u64());
    out.authority.critic = CriticId::from_value(authority_reader.u64());
    out.authority.critic_generation = CriticGeneration::from_value(authority_reader.u64());
    out.authority.review = ReviewId::from_value(authority_reader.u64());
    out.authority.review_generation = ReviewGeneration::from_value(authority_reader.u64());
    out.authority.target = TargetId::from_value(authority_reader.u64());
    out.authority.target_generation = TargetGeneration::from_value(authority_reader.u64());
    out.authority.request = RequestId::from_value(authority_reader.u64());
    if (!authority_reader.ok()) {
        return authority_reader.status();
    }
    out.checksum = read_u32_be(data + 104);
    return Status::success();
}

Status FrameCodec::decode_payload(const FrameHeader& header, const std::uint8_t* data, std::size_t len,
                                  const RuntimeLimits& limits, std::vector<std::uint8_t>& out) {
    // len is the total frame length: header plus payload.
    const std::size_t expected = kHeaderBytes + header.payload_length;
    if (len < expected) {
        return Status::error(ErrorCode::ProtocolTruncated, "frame payload is shorter than its header declares");
    }
    if (len > expected) {
        return Status::error(ErrorCode::ProtocolTrailingGarbage,
                             "buffer carries bytes after the declared frame");
    }
    if (header.payload_length > limits.max_frame_bytes) {
        return Status::error(ErrorCode::ProtocolOversized, "frame payload exceeds the configured frame bound");
    }
    // The checksum covers the header fields that precede the checksum itself,
    // followed by the payload, so a corrupted length or authority field is
    // detected exactly like a corrupted payload.
    const std::uint32_t header_crc = crc32(data, FrameCodec::kChecksumCoverageBytes);
    const std::uint32_t full_crc = crc32_append(header_crc, data + FrameCodec::kHeaderBytes, header.payload_length);
    if (full_crc != header.checksum) {
        return Status::error(ErrorCode::ProtocolBadChecksum, "frame checksum does not match");
    }
    out.assign(data + FrameCodec::kHeaderBytes, data + FrameCodec::kHeaderBytes + header.payload_length);
    return Status::success();
}

Status FrameCodec::decode(const std::vector<std::uint8_t>& buffer, const RuntimeLimits& limits, Frame& out) {
    FrameHeader header;
    Status status = decode_header(buffer.data(), buffer.size(), limits, header);
    if (!status.ok()) {
        return status;
    }
    if (buffer.size() < kHeaderBytes + header.payload_length) {
        return Status::error(ErrorCode::ProtocolTruncated, "frame payload is incomplete");
    }
    if (buffer.size() > kHeaderBytes + header.payload_length) {
        return Status::error(ErrorCode::ProtocolTrailingGarbage, "buffer carries bytes after the declared frame");
    }
    status = decode_payload(header, buffer.data(), buffer.size(), limits, out.payload);
    if (!status.ok()) {
        return status;
    }
    out.header = header;
    return Status::success();
}

// --- Payload codecs ----------------------------------------------------------

void encode(ByteWriter& writer, const StatusEnvelope& value) {
    writer.u32(static_cast<std::uint32_t>(value.code));
    writer.string(value.detail, kHardMaxMetadataBytes);
}

Status decode(ByteReader& reader, StatusEnvelope& out, const RuntimeLimits& limits) {
    const std::uint32_t raw = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (raw > static_cast<std::uint32_t>(ErrorCode::InternalError)) {
        return Status::error(ErrorCode::ProtocolInvalidEnum, "status envelope carries an unknown error code");
    }
    out.code = static_cast<ErrorCode>(raw);
    out.detail = reader.string(limits.max_metadata_string_bytes);
    return reader.status();
}

void encode(ByteWriter& writer, const RegisterTargetRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.id.value());
    writer.u32(static_cast<std::uint32_t>(value.target_class));
    writer.string(value.schema_name, limits.max_metadata_string_bytes);
    writer.bytes(value.payload, limits.max_target_payload_bytes);
    writer.u64(value.producer.value());
    writer.string(value.producer_version, limits.max_metadata_string_bytes);
    writer.string(value.provenance, limits.max_metadata_string_bytes);
    writer.boolean(value.reviewable);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, RegisterTargetRequest& out) {
    out.id = TargetId::from_value(reader.u64());
    out.target_class = read_enum<TargetClass>(reader, is_valid_target_class, "target_class");
    out.schema_name = reader.string(limits.max_metadata_string_bytes);
    out.payload = reader.bytes(limits.max_target_payload_bytes);
    out.producer = ProducerId::from_value(reader.u64());
    out.producer_version = reader.string(limits.max_metadata_string_bytes);
    out.provenance = reader.string(limits.max_metadata_string_bytes);
    out.reviewable = reader.boolean();
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const SupersedeTargetRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.target.value());
    writer.u32(static_cast<std::uint32_t>(value.target_class));
    writer.string(value.schema_name, limits.max_metadata_string_bytes);
    writer.bytes(value.payload, limits.max_target_payload_bytes);
    writer.u64(value.producer.value());
    writer.string(value.producer_version, limits.max_metadata_string_bytes);
    writer.string(value.provenance, limits.max_metadata_string_bytes);
    writer.string(value.reason, limits.max_metadata_string_bytes);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, SupersedeTargetRequest& out) {
    out.target = TargetId::from_value(reader.u64());
    out.target_class = read_enum<TargetClass>(reader, is_valid_target_class, "target_class");
    out.schema_name = reader.string(limits.max_metadata_string_bytes);
    out.payload = reader.bytes(limits.max_target_payload_bytes);
    out.producer = ProducerId::from_value(reader.u64());
    out.producer_version = reader.string(limits.max_metadata_string_bytes);
    out.provenance = reader.string(limits.max_metadata_string_bytes);
    out.reason = reader.string(limits.max_metadata_string_bytes);
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const RegisterCriticRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.id.value());
    encode(writer, value.capability);
    writer.u64(value.registrant.value());
    writer.string(value.provenance, limits.max_metadata_string_bytes);
    encode(writer, value.compat);
    writer.boolean(value.advance_generation);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, RegisterCriticRequest& out) {
    out.id = CriticId::from_value(reader.u64());
    Status status = decode(reader, out.capability, limits);
    if (!status.ok()) {
        return status;
    }
    out.registrant = ProducerId::from_value(reader.u64());
    out.provenance = reader.string(limits.max_metadata_string_bytes);
    status = decode(reader, out.compat);
    if (!status.ok()) {
        return status;
    }
    out.advance_generation = reader.boolean();
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const RegisterWorkerRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.id.value());
    writer.u64(value.boot.value());
    writer.u64(value.critic.value());
    writer.u64(value.critic_generation.value());
    writer.u64(value.host.value());
    writer.string(value.process_label, limits.max_metadata_string_bytes);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, RegisterWorkerRequest& out) {
    out.id = WorkerId::from_value(reader.u64());
    out.boot = WorkerBootId::from_value(reader.u64());
    out.critic = CriticId::from_value(reader.u64());
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.host = ProducerId::from_value(reader.u64());
    out.process_label = reader.string(limits.max_metadata_string_bytes);
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const DeclareReadinessRequest& value, const RuntimeLimits& limits) {
    (void)limits;
    writer.u64(value.id.value());
    writer.u64(value.boot.value());
    writer.u64(value.critic.value());
    writer.u64(value.critic_generation.value());
    writer.boolean(value.ready);
    writer.boolean(value.healthy);
    writer.digest(value.capability_evidence);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, DeclareReadinessRequest& out) {
    (void)limits;
    out.id = WorkerId::from_value(reader.u64());
    out.boot = WorkerBootId::from_value(reader.u64());
    out.critic = CriticId::from_value(reader.u64());
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.ready = reader.boolean();
    out.healthy = reader.boolean();
    out.capability_evidence = reader.digest();
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const DeclareReviewRequest& value, const RuntimeLimits& limits) {
    (void)limits;
    writer.u64(value.request.value());
    writer.u64(value.target.value());
    writer.u64(value.target_generation.value());
    encode(writer, value.spec);
    writer.u64(value.epoch.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, DeclareReviewRequest& out) {
    out.request = ReviewRequestId::from_value(reader.u64());
    out.target = TargetId::from_value(reader.u64());
    out.target_generation = TargetGeneration::from_value(reader.u64());
    Status status = decode(reader, out.spec, limits);
    if (!status.ok()) {
        return status;
    }
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const AssignCriticsRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    if (value.assignments.size() > limits.max_critics_per_review) {
        writer.fail(ErrorCode::ResourceLimitExceeded, "assignment batch exceeds the configured critic bound");
        return;
    }
    writer.u32(static_cast<std::uint32_t>(value.assignments.size()));
    for (const AssignCriticsRequest::Assignment& assignment : value.assignments) {
        writer.u64(assignment.critic.value());
        writer.u64(assignment.critic_generation.value());
        writer.u64(assignment.worker.value());
        writer.u64(assignment.worker_boot.value());
        writer.u32(static_cast<std::uint32_t>(assignment.role));
        writer.boolean(assignment.required);
    }
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, AssignCriticsRequest& out) {
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > limits.max_critics_per_review) {
        return Status::error(ErrorCode::ProtocolOversized, "assignment batch exceeds the configured critic bound");
    }
    out.assignments.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        AssignCriticsRequest::Assignment assignment;
        assignment.critic = CriticId::from_value(reader.u64());
        assignment.critic_generation = CriticGeneration::from_value(reader.u64());
        assignment.worker = WorkerId::from_value(reader.u64());
        assignment.worker_boot = WorkerBootId::from_value(reader.u64());
        assignment.role = read_enum<CriticRole>(reader, is_valid_critic_role, "critic_role");
        assignment.required = reader.boolean();
        if (!reader.ok()) {
            return reader.status();
        }
        out.assignments.push_back(assignment);
    }
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const SubmitEvidenceRequest& value, const RuntimeLimits& limits) {
    writer.u32(static_cast<std::uint32_t>(value.source_type));
    writer.u32(static_cast<std::uint32_t>(value.provenance));
    writer.u32(static_cast<std::uint32_t>(value.integrity));
    writer.u64(value.producer.value());
    writer.string(value.reference, limits.max_metadata_string_bytes);
    writer.bytes(value.payload, limits.max_evidence_payload_bytes);
    writer.u64(value.finding.value());
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    writer.u64(value.target.value());
    writer.u64(value.target_generation.value());
    writer.u64(value.critic.value());
    writer.u64(value.critic_generation.value());
    writer.u64(value.worker.value());
    writer.u64(value.worker_boot.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitEvidenceRequest& out) {
    out.source_type = read_enum<EvidenceSourceType>(reader, is_valid_evidence_source_type, "evidence_source_type");
    out.provenance = read_enum<EvidenceProvenance>(reader, is_valid_evidence_provenance, "evidence_provenance");
    out.integrity = read_enum<EvidenceIntegrity>(reader, is_valid_evidence_integrity, "evidence_integrity");
    out.producer = ProducerId::from_value(reader.u64());
    out.reference = reader.string(limits.max_metadata_string_bytes);
    out.payload = reader.bytes(limits.max_evidence_payload_bytes);
    out.finding = FindingId::from_value(reader.u64());
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = TargetId::from_value(reader.u64());
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.critic = CriticId::from_value(reader.u64());
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.worker = WorkerId::from_value(reader.u64());
    out.worker_boot = WorkerBootId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const SubmitFindingRequest& value, const RuntimeLimits& limits) {
    writer.u32(static_cast<std::uint32_t>(value.category));
    writer.u32(static_cast<std::uint32_t>(value.severity));
    writer.u32(static_cast<std::uint32_t>(value.outcome));
    encode_id_list(writer, value.evidence, limits.max_evidence_per_finding);
    writer.string(value.explanation, limits.max_explanation_bytes);
    encode(writer, value.confidence);
    writer.string(value.provenance, limits.max_metadata_string_bytes);
    writer.u64(value.supersedes.value());
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    writer.u64(value.target.value());
    writer.u64(value.target_generation.value());
    writer.u64(value.critic.value());
    writer.u64(value.critic_generation.value());
    writer.u64(value.worker.value());
    writer.u64(value.worker_boot.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitFindingRequest& out) {
    out.category = read_enum<FindingCategory>(reader, is_valid_finding_category, "finding_category");
    out.severity = read_enum<Severity>(reader, is_valid_severity, "severity");
    out.outcome = read_enum<FindingOutcome>(reader, is_valid_finding_outcome, "finding_outcome");
    Status status = decode_id_list(reader, out.evidence, limits.max_evidence_per_finding);
    if (!status.ok()) {
        return status;
    }
    out.explanation = reader.string(limits.max_explanation_bytes);
    status = decode(reader, out.confidence);
    if (!status.ok()) {
        return status;
    }
    out.provenance = reader.string(limits.max_metadata_string_bytes);
    out.supersedes = FindingId::from_value(reader.u64());
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = TargetId::from_value(reader.u64());
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.critic = CriticId::from_value(reader.u64());
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.worker = WorkerId::from_value(reader.u64());
    out.worker_boot = WorkerBootId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const SubmitChallengeRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.challenged_finding.value());
    writer.u64(value.challenged_finding_generation.value());
    writer.string(value.disputed_predicate, limits.max_metadata_string_bytes);
    writer.string(value.rationale, limits.max_explanation_bytes);
    encode_id_list(writer, value.evidence, limits.max_evidence_per_finding);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    writer.u64(value.target.value());
    writer.u64(value.target_generation.value());
    writer.u64(value.critic.value());
    writer.u64(value.critic_generation.value());
    writer.u64(value.worker.value());
    writer.u64(value.worker_boot.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitChallengeRequest& out) {
    out.challenged_finding = FindingId::from_value(reader.u64());
    out.challenged_finding_generation = FindingGeneration::from_value(reader.u64());
    out.disputed_predicate = reader.string(limits.max_metadata_string_bytes);
    out.rationale = reader.string(limits.max_explanation_bytes);
    Status status = decode_id_list(reader, out.evidence, limits.max_evidence_per_finding);
    if (!status.ok()) {
        return status;
    }
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = TargetId::from_value(reader.u64());
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.critic = CriticId::from_value(reader.u64());
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.worker = WorkerId::from_value(reader.u64());
    out.worker_boot = WorkerBootId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const SubmitRebuttalRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.challenge.value());
    writer.u64(value.challenge_generation.value());
    writer.u32(static_cast<std::uint32_t>(value.outcome));
    writer.string(value.argument, limits.max_explanation_bytes);
    encode_id_list(writer, value.evidence, limits.max_evidence_per_finding);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    writer.u64(value.target.value());
    writer.u64(value.target_generation.value());
    writer.u64(value.critic.value());
    writer.u64(value.critic_generation.value());
    writer.u64(value.worker.value());
    writer.u64(value.worker_boot.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, SubmitRebuttalRequest& out) {
    out.challenge = ChallengeId::from_value(reader.u64());
    out.challenge_generation = ChallengeGeneration::from_value(reader.u64());
    out.outcome = read_enum<ChallengeOutcome>(reader, is_valid_challenge_outcome, "challenge_outcome");
    out.argument = reader.string(limits.max_explanation_bytes);
    Status status = decode_id_list(reader, out.evidence, limits.max_evidence_per_finding);
    if (!status.ok()) {
        return status;
    }
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = TargetId::from_value(reader.u64());
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.critic = CriticId::from_value(reader.u64());
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.worker = WorkerId::from_value(reader.u64());
    out.worker_boot = WorkerBootId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const CommitReviewRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    writer.u64(value.target_generation.value());
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
    writer.boolean(value.include_rendered_explanation);
    (void)limits;
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, CommitReviewRequest& out) {
    (void)limits;
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    out.include_rendered_explanation = reader.boolean();
    return reader.status();
}

void encode(ByteWriter& writer, const CancelReviewRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    writer.string(value.reason, limits.max_metadata_string_bytes);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, CancelReviewRequest& out) {
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.reason = reader.string(limits.max_metadata_string_bytes);
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

void encode(ByteWriter& writer, const ReviewSnapshot& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value.review);
    writer.u32(static_cast<std::uint32_t>(value.assignments.size()));
    for (const CriticAssignment& assignment : value.assignments) {
        encode(writer, assignment);
    }
    writer.u32(static_cast<std::uint32_t>(value.findings.size()));
    for (const Finding& finding : value.findings) {
        encode(writer, finding);
    }
    writer.u32(static_cast<std::uint32_t>(value.evidences.size()));
    for (const EvidenceRecord& evidence : value.evidences) {
        encode(writer, evidence);
    }
    writer.u32(static_cast<std::uint32_t>(value.challenges.size()));
    for (const Challenge& challenge : value.challenges) {
        encode(writer, challenge);
    }
    writer.u32(static_cast<std::uint32_t>(value.rebuttals.size()));
    for (const Rebuttal& rebuttal : value.rebuttals) {
        encode(writer, rebuttal);
    }
    writer.boolean(value.decision.has_value());
    if (value.decision.has_value()) {
        encode(writer, *value.decision);
    }
    writer.digest(value.state_digest);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, ReviewSnapshot& out) {
    Status status = decode(reader, out.review, limits);
    if (!status.ok()) {
        return status;
    }
    std::uint32_t count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > limits.max_critics_per_review * 4u) {
        return Status::error(ErrorCode::ProtocolOversized, "snapshot assignment list exceeds the bound");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        CriticAssignment assignment;
        status = decode(reader, assignment, limits);
        if (!status.ok()) {
            return status;
        }
        out.assignments.push_back(std::move(assignment));
    }
    count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > limits.max_findings_per_review * 4u) {
        return Status::error(ErrorCode::ProtocolOversized, "snapshot finding list exceeds the bound");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Finding finding;
        status = decode(reader, finding, limits);
        if (!status.ok()) {
            return status;
        }
        out.findings.push_back(std::move(finding));
    }
    count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > limits.max_evidence_records_per_review * 4u) {
        return Status::error(ErrorCode::ProtocolOversized, "snapshot evidence list exceeds the bound");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        EvidenceRecord evidence;
        status = decode(reader, evidence, limits);
        if (!status.ok()) {
            return status;
        }
        out.evidences.push_back(std::move(evidence));
    }
    count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > limits.max_challenges_per_review * 4u) {
        return Status::error(ErrorCode::ProtocolOversized, "snapshot challenge list exceeds the bound");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Challenge challenge;
        status = decode(reader, challenge, limits);
        if (!status.ok()) {
            return status;
        }
        out.challenges.push_back(std::move(challenge));
    }
    count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > limits.max_challenges_per_review * 4u) {
        return Status::error(ErrorCode::ProtocolOversized, "snapshot rebuttal list exceeds the bound");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        Rebuttal rebuttal;
        status = decode(reader, rebuttal, limits);
        if (!status.ok()) {
            return status;
        }
        out.rebuttals.push_back(std::move(rebuttal));
    }
    if (reader.boolean()) {
        Decision decision;
        status = decode(reader, decision);
        if (!status.ok()) {
            return status;
        }
        out.decision = decision;
    }
    if (!reader.ok()) {
        return reader.status();
    }
    out.state_digest = reader.digest();
    return reader.status();
}

void encode(ByteWriter& writer, const ReviewSummary& value, const RuntimeLimits& limits) {
    (void)limits;
    writer.u64(value.id.value());
    writer.u64(value.generation.value());
    writer.u64(value.target.value());
    writer.u64(value.target_generation.value());
    writer.u32(static_cast<std::uint32_t>(value.state));
    writer.u32(static_cast<std::uint32_t>(value.disposition));
    writer.u32(value.finding_count);
    writer.u32(value.current_finding_count);
    writer.u32(value.assignment_count);
    writer.boolean(value.has_decision);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, ReviewSummary& out) {
    (void)limits;
    out.id = ReviewId::from_value(reader.u64());
    out.generation = ReviewGeneration::from_value(reader.u64());
    out.target = TargetId::from_value(reader.u64());
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.state = read_enum<ReviewState>(reader, is_valid_review_state, "review_state");
    out.disposition = read_enum<ReviewDisposition>(reader, is_valid_review_disposition, "review_disposition");
    out.finding_count = reader.u32();
    out.current_finding_count = reader.u32();
    out.assignment_count = reader.u32();
    out.has_decision = reader.boolean();
    return reader.status();
}


// --- Typed pass-through codecs ----------------------------------------------
// These reuse the canonical domain encoding so a wire value and a durable value
// are byte-identical for the same object.

void encode(ByteWriter& writer, const CriticAssignment& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, CriticAssignment& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const WorkerRecord& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, WorkerRecord& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const Decision& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, Decision& out) {
    (void)limits;
    return decode(reader, out);
}

void encode(ByteWriter& writer, const Finding& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, Finding& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const TargetRecord& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, TargetRecord& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const Challenge& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, Challenge& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const Rebuttal& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, Rebuttal& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const EvidenceRecord& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, EvidenceRecord& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const CriticRegistration& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, CriticRegistration& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const ReviewRecord& value, const RuntimeLimits& limits) {
    (void)limits;
    encode(writer, value);
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, ReviewRecord& out) {
    return decode(reader, out, limits);
}

void encode(ByteWriter& writer, const FenceWorkerRequest& value, const RuntimeLimits& limits) {
    writer.u64(value.worker.value());
    writer.string(value.reason, limits.max_metadata_string_bytes);
    writer.u64(value.epoch.value());
    writer.u64(value.request.value());
}

Status decode(ByteReader& reader, const RuntimeLimits& limits, FenceWorkerRequest& out) {
    out.worker = WorkerId::from_value(reader.u64());
    out.reason = reader.string(limits.max_metadata_string_bytes);
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.request = RequestId::from_value(reader.u64());
    return reader.status();
}

}  // namespace critic_fabric

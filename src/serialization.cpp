// Critic Fabric — canonical checked byte encoding implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/serialization.hpp"

#include <cstring>
#include <limits>
#include <utility>

#include "critic_fabric/limits.hpp"

namespace critic_fabric {
namespace {

constexpr std::uint32_t kSpecVersion = 1;

std::uint32_t checked_item_count(std::size_t count, std::uint32_t max_items, ByteWriter& writer,
                                 const char* what) {
    if (count > max_items) {
        writer.fail(ErrorCode::ResourceLimitExceeded,
                    std::string("too many ") + what + " items in encoded value");
        return 0;
    }
    return static_cast<std::uint32_t>(count);
}

void write_item_count(ByteWriter& writer, std::size_t count, std::uint32_t max_items, const char* what) {
    const std::uint32_t bounded = checked_item_count(count, max_items, writer, what);
    if (writer.ok()) {
        writer.u32(bounded);
    }
}

// Reads a collection length and rejects anything above the caller's bound
// before a single element is reserved.
bool begin_items(ByteReader& reader, std::uint32_t max_items, const char* what, std::uint32_t& out) {
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) {
        return false;
    }
    if (count > max_items) {
        reader.fail(reader.policy().oversized, std::string("too many ") + what + " items");
        return false;
    }
    out = count;
    return true;
}

void write_roles(ByteWriter& writer, const std::set<CriticRole>& roles, std::uint32_t max_items) {
    write_item_count(writer, roles.size(), max_items, "role");
    for (CriticRole role : roles) {
        write_enum(writer, role);
    }
}

Status read_roles(ByteReader& reader, std::set<CriticRole>& out, std::uint32_t max_items) {
    std::uint32_t count = 0;
    if (!begin_items(reader, max_items, "role", count)) {
        return reader.status();
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        out.insert(read_enum<CriticRole>(reader, is_valid_critic_role, "critic_role"));
    }
    return reader.status();
}

void write_target_classes(ByteWriter& writer, const std::set<TargetClass>& values, std::uint32_t max_items) {
    write_item_count(writer, values.size(), max_items, "target class");
    for (TargetClass value : values) {
        write_enum(writer, value);
    }
}

Status read_target_classes(ByteReader& reader, std::set<TargetClass>& out, std::uint32_t max_items) {
    std::uint32_t count = 0;
    if (!begin_items(reader, max_items, "target class", count)) {
        return reader.status();
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        out.insert(read_enum<TargetClass>(reader, is_valid_target_class, "target_class"));
    }
    return reader.status();
}

void write_categories(ByteWriter& writer, const std::set<FindingCategory>& values, std::uint32_t max_items) {
    write_item_count(writer, values.size(), max_items, "finding category");
    for (FindingCategory value : values) {
        write_enum(writer, value);
    }
}

Status read_categories(ByteReader& reader, std::set<FindingCategory>& out, std::uint32_t max_items) {
    std::uint32_t count = 0;
    if (!begin_items(reader, max_items, "finding category", count)) {
        return reader.status();
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        out.insert(read_enum<FindingCategory>(reader, is_valid_finding_category, "finding_category"));
    }
    return reader.status();
}

void write_role_list(ByteWriter& writer, const std::vector<CriticRole>& roles, std::uint32_t max_items) {
    write_item_count(writer, roles.size(), max_items, "role");
    for (CriticRole role : roles) {
        write_enum(writer, role);
    }
}

Status read_role_list(ByteReader& reader, std::vector<CriticRole>& out, std::uint32_t max_items) {
    std::uint32_t count = 0;
    if (!begin_items(reader, max_items, "role", count)) {
        return reader.status();
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(read_enum<CriticRole>(reader, is_valid_critic_role, "critic_role"));
    }
    return reader.status();
}

void write_id_list(ByteWriter& writer, const std::vector<EvidenceId>& ids, std::uint32_t max_items) {
    write_item_count(writer, ids.size(), max_items, "evidence id");
    for (EvidenceId id : ids) {
        writer.u64(id.value());
    }
}

Status read_id_list(ByteReader& reader, std::vector<EvidenceId>& out, std::uint32_t max_items) {
    std::uint32_t count = 0;
    if (!begin_items(reader, max_items, "evidence id", count)) {
        return reader.status();
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(EvidenceId::from_value(reader.u64()));
    }
    return reader.status();
}

}  // namespace

// --- ByteWriter --------------------------------------------------------------

void ByteWriter::require(std::size_t additional) {
    if (!ok_) {
        return;
    }
    const std::size_t max_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    if (additional > max_size || buffer_.size() > max_size - additional) {
        fail(ErrorCode::ResourceLimitExceeded, "encoded value exceeds the addressable byte range");
    }
}

void ByteWriter::fail(ErrorCode code, std::string detail) {
    if (ok_) {
        ok_ = false;
        code_ = code;
        detail_ = std::move(detail);
    }
}

void ByteWriter::reserve(std::size_t bytes) {
    if (!ok_) {
        return;
    }
    buffer_.reserve(bytes);
}

void ByteWriter::u8(std::uint8_t value) {
    require(1);
    if (ok_) {
        buffer_.push_back(value);
    }
}

void ByteWriter::u16(std::uint16_t value) {
    require(2);
    if (!ok_) {
        return;
    }
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void ByteWriter::u32(std::uint32_t value) {
    require(4);
    if (!ok_) {
        return;
    }
    for (int shift = 24; shift >= 0; shift -= 8) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void ByteWriter::u64(std::uint64_t value) {
    require(8);
    if (!ok_) {
        return;
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::boolean(bool value) { u8(value ? 1u : 0u); }

void ByteWriter::raw(const std::uint8_t* data, std::size_t len) {
    if (len == 0) {
        return;
    }
    if (data == nullptr) {
        fail(ErrorCode::InternalError, "null source passed to ByteWriter::raw");
        return;
    }
    require(len);
    if (!ok_) {
        return;
    }
    buffer_.insert(buffer_.end(), data, data + len);
}

void ByteWriter::digest(const Digest& value) {
    require(Digest::kBytes);
    if (!ok_) {
        return;
    }
    buffer_.insert(buffer_.end(), value.bytes().begin(), value.bytes().end());
}

void ByteWriter::string(std::string_view value, std::uint32_t max_bytes) {
    if (value.size() > max_bytes) {
        fail(ErrorCode::ResourceLimitExceeded, "string field exceeds the configured metadata bound");
        return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    raw(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

void ByteWriter::bytes(const std::vector<std::uint8_t>& value, std::uint32_t max_bytes) {
    if (value.size() > max_bytes) {
        fail(ErrorCode::ResourceLimitExceeded, "byte field exceeds the configured payload bound");
        return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    raw(value.data(), value.size());
}

void ByteWriter::patch_u32(std::size_t offset, std::uint32_t value) {
    if (!ok_ || offset + 4 > buffer_.size()) {
        fail(ErrorCode::InternalError, "patch offset outside the written buffer");
        return;
    }
    for (int i = 0; i < 4; ++i) {
        buffer_[offset + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (24 - 8 * i)) & 0xFFu);
    }
}

Status ByteWriter::status() const {
    if (ok_) {
        return Status::success();
    }
    return Status::error(code_, detail_);
}

// --- ByteReader --------------------------------------------------------------

ByteReader::ByteReader(const std::uint8_t* data, std::size_t len, ReaderPolicy policy)
    : data_(data), len_(len), policy_(policy) {
    if (data == nullptr && len != 0) {
        fail(policy_.malformed, "null buffer with non-zero length");
    }
}

ByteReader::ByteReader(const std::vector<std::uint8_t>& data, ReaderPolicy policy)
    : ByteReader(data.data(), data.size(), policy) {}

bool ByteReader::need(std::size_t count) {
    if (!ok_) {
        return false;
    }
    if (count > len_ - pos_) {
        fail(policy_.truncated, "input ended before the declared field was complete");
        return false;
    }
    return true;
}

void ByteReader::fail(ErrorCode code, std::string detail) {
    if (ok_) {
        ok_ = false;
        code_ = code;
        detail_ = std::move(detail);
    }
}

Status ByteReader::status() const {
    if (ok_) {
        return Status::success();
    }
    return Status::error(code_, detail_);
}

std::uint8_t ByteReader::u8() {
    if (!need(1)) {
        return 0;
    }
    return data_[pos_++];
}

std::uint16_t ByteReader::u16() {
    if (!need(2)) {
        return 0;
    }
    const std::uint16_t value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data_[pos_]) << 8) | static_cast<std::uint16_t>(data_[pos_ + 1]));
    pos_ += 2;
    return value;
}

std::uint32_t ByteReader::u32() {
    if (!need(4)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<std::uint32_t>(data_[pos_ + static_cast<std::size_t>(i)]);
    }
    pos_ += 4;
    return value;
}

std::uint64_t ByteReader::u64() {
    if (!need(8)) {
        return 0;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(data_[pos_ + static_cast<std::size_t>(i)]);
    }
    pos_ += 8;
    return value;
}

std::int64_t ByteReader::i64() { return static_cast<std::int64_t>(u64()); }

bool ByteReader::boolean() {
    const std::uint8_t raw = u8();
    if (!ok_) {
        return false;
    }
    if (raw > 1u) {
        fail(policy_.malformed, "boolean field carried a value other than 0 or 1");
        return false;
    }
    return raw == 1u;
}

Digest ByteReader::digest() {
    Digest value;
    if (!need(Digest::kBytes)) {
        return value;
    }
    std::memcpy(value.bytes().data(), cursor(), Digest::kBytes);
    pos_ += Digest::kBytes;
    return value;
}

std::string ByteReader::string(std::uint32_t max_bytes) {
    const std::uint32_t len = u32();
    if (!ok_) {
        return {};
    }
    if (len > max_bytes) {
        fail(policy_.oversized, "string field exceeds the configured metadata bound");
        return {};
    }
    if (!need(len)) {
        return {};
    }
    std::string value(reinterpret_cast<const char*>(cursor()), len);
    pos_ += len;
    return value;
}

std::vector<std::uint8_t> ByteReader::bytes(std::uint32_t max_bytes) {
    const std::uint32_t len = u32();
    if (!ok_) {
        return {};
    }
    if (len > max_bytes) {
        fail(policy_.oversized, "byte field exceeds the configured payload bound");
        return {};
    }
    if (!need(len)) {
        return {};
    }
    std::vector<std::uint8_t> value(cursor(), cursor() + len);
    pos_ += len;
    return value;
}

bool ByteReader::skip(std::size_t count) {
    if (!need(count)) {
        return false;
    }
    pos_ += count;
    return true;
}

// --- Domain encoders ---------------------------------------------------------

void encode(ByteWriter& writer, const Digest& value) { writer.digest(value); }

void encode(ByteWriter& writer, const Compatibility& value) {
    writer.string(value.schema_name, kHardMaxMetadataBytes);
    writer.string(value.producer_version, kHardMaxMetadataBytes);
}

Status decode(ByteReader& reader, Compatibility& out) {
    out.schema_name = reader.string(kHardMaxMetadataBytes);
    out.producer_version = reader.string(kHardMaxMetadataBytes);
    return reader.status();
}

void encode(ByteWriter& writer, const TargetRecord& value) {
    write_id(writer, value.id);
    writer.u64(value.generation.value());
    write_enum(writer, value.target_class);
    writer.digest(value.content_digest);
    writer.digest(value.schema_digest);
    writer.string(value.schema_name, kHardMaxMetadataBytes);
    write_id(writer, value.producer);
    writer.string(value.producer_version, kHardMaxMetadataBytes);
    writer.string(value.provenance, kHardMaxMetadataBytes);
    writer.bytes(value.payload, kHardMaxPayloadBytes);
    writer.u64(value.created_order);
    writer.boolean(value.reviewable);
    write_enum(writer, value.supersession);
    writer.u64(value.superseded_by.value());
    writer.string(value.supersession_reason, kHardMaxMetadataBytes);
}

Status decode(ByteReader& reader, TargetRecord& out, const RuntimeLimits& limits) {
    out.id = read_id<TargetId>(reader);
    out.generation = TargetGeneration::from_value(reader.u64());
    out.target_class = read_enum<TargetClass>(reader, is_valid_target_class, "target_class");
    out.content_digest = reader.digest();
    out.schema_digest = reader.digest();
    out.schema_name = reader.string(limits.max_metadata_string_bytes);
    out.producer = read_id<ProducerId>(reader);
    out.producer_version = reader.string(limits.max_metadata_string_bytes);
    out.provenance = reader.string(limits.max_metadata_string_bytes);
    out.payload = reader.bytes(limits.max_target_payload_bytes);
    out.created_order = reader.u64();
    out.reviewable = reader.boolean();
    out.supersession = read_enum<TargetSupersession>(reader, is_valid_target_supersession, "target_supersession");
    out.superseded_by = TargetGeneration::from_value(reader.u64());
    out.supersession_reason = reader.string(limits.max_metadata_string_bytes);
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.generation.valid()) {
        return Status::error(reader.policy().malformed, "target record carried an invalid identity or generation");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const CriticCapability& value) {
    write_roles(writer, value.roles, kHardMaxMetadataBytes);
    write_target_classes(writer, value.target_classes, kTargetClassMax);
    write_categories(writer, value.finding_categories, kFindingCategoryMax);
    writer.string(value.specialization, kHardMaxMetadataBytes);
    writer.string(value.implementation_version, kHardMaxMetadataBytes);
    writer.digest(value.capability_evidence);
    writer.boolean(value.deterministic_output);
    writer.boolean(value.supports_challenge_response);
}

Status decode(ByteReader& reader, CriticCapability& out, const RuntimeLimits& limits) {
    (void)limits;
    Status status = read_roles(reader, out.roles, kCriticRoleMax);
    if (!status.ok()) {
        return status;
    }
    status = read_target_classes(reader, out.target_classes, kTargetClassMax);
    if (!status.ok()) {
        return status;
    }
    status = read_categories(reader, out.finding_categories, kFindingCategoryMax);
    if (!status.ok()) {
        return status;
    }
    out.specialization = reader.string(kHardMaxMetadataBytes);
    out.implementation_version = reader.string(kHardMaxMetadataBytes);
    out.capability_evidence = reader.digest();
    out.deterministic_output = reader.boolean();
    out.supports_challenge_response = reader.boolean();
    return reader.status();
}

void encode(ByteWriter& writer, const CriticRegistration& value) {
    write_id(writer, value.id);
    writer.u64(value.generation.value());
    encode(writer, value.capability);
    write_id(writer, value.registrant);
    writer.string(value.provenance, kHardMaxMetadataBytes);
    encode(writer, value.compat);
    writer.u64(value.registered_order);
}

Status decode(ByteReader& reader, CriticRegistration& out, const RuntimeLimits& limits) {
    out.id = read_id<CriticId>(reader);
    out.generation = CriticGeneration::from_value(reader.u64());
    Status status = decode(reader, out.capability, limits);
    if (!status.ok()) {
        return status;
    }
    out.registrant = read_id<ProducerId>(reader);
    out.provenance = reader.string(limits.max_metadata_string_bytes);
    status = decode(reader, out.compat);
    if (!status.ok()) {
        return status;
    }
    out.registered_order = reader.u64();
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.generation.valid()) {
        return Status::error(reader.policy().malformed, "critic registration carried an invalid identity or generation");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const WorkerRecord& value) {
    write_id(writer, value.id);
    write_id(writer, value.boot);
    writer.u64(value.epoch.value());
    write_id(writer, value.critic);
    writer.u64(value.critic_generation.value());
    write_id(writer, value.host);
    writer.string(value.process_label, kHardMaxMetadataBytes);
    writer.boolean(value.ready);
    writer.boolean(value.healthy);
    writer.boolean(value.authoritative);
    writer.u64(value.boot_order);
    writer.u64(value.readiness_order);
    writer.digest(value.readiness_digest);
}

Status decode(ByteReader& reader, WorkerRecord& out, const RuntimeLimits& limits) {
    out.id = read_id<WorkerId>(reader);
    out.boot = read_id<WorkerBootId>(reader);
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.critic = read_id<CriticId>(reader);
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.host = read_id<ProducerId>(reader);
    out.process_label = reader.string(limits.max_metadata_string_bytes);
    out.ready = reader.boolean();
    out.healthy = reader.boolean();
    out.authoritative = reader.boolean();
    out.boot_order = reader.u64();
    out.readiness_order = reader.u64();
    out.readiness_digest = reader.digest();
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.boot.valid()) {
        return Status::error(reader.policy().malformed, "worker record carried an invalid identity or boot");
    }
    if (out.boot_order == 0) {
        return Status::error(reader.policy().malformed, "worker record carried no boot ordering");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const Confidence& value) {
    writer.boolean(value.provided);
    writer.u32(value.basis_points);
    write_enum(writer, value.basis);
}

Status decode(ByteReader& reader, Confidence& out) {
    out.provided = reader.boolean();
    out.basis_points = reader.u32();
    out.basis = read_enum<ConfidenceBasis>(reader, is_valid_confidence_basis, "confidence_basis");
    if (!reader.ok()) {
        return reader.status();
    }
    if (out.basis_points > 10000u) {
        return Status::error(reader.policy().malformed, "confidence basis points outside 0..10000");
    }
    if (!out.provided && out.basis != ConfidenceBasis::NotProvided) {
        return Status::error(reader.policy().malformed, "confidence basis supplied without a provided flag");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const EvidenceRecord& value) {
    write_id(writer, value.id);
    writer.u64(value.generation.value());
    write_enum(writer, value.source_type);
    write_enum(writer, value.provenance);
    write_enum(writer, value.integrity);
    write_id(writer, value.producer);
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    writer.digest(value.target_digest);
    write_id(writer, value.review);
    writer.u64(value.review_generation.value());
    write_id(writer, value.critic);
    writer.u64(value.critic_generation.value());
    write_id(writer, value.worker);
    write_id(writer, value.worker_boot);
    write_id(writer, value.finding);
    writer.digest(value.content_digest);
    writer.bytes(value.payload, kHardMaxPayloadBytes);
    writer.string(value.reference, kHardMaxMetadataBytes);
    writer.u64(value.created_order);
    writer.boolean(value.current);
    writer.boolean(value.superseded);
    write_id(writer, value.superseded_by);
}

Status decode(ByteReader& reader, EvidenceRecord& out, const RuntimeLimits& limits) {
    out.id = read_id<EvidenceId>(reader);
    out.generation = EvidenceGeneration::from_value(reader.u64());
    out.source_type = read_enum<EvidenceSourceType>(reader, is_valid_evidence_source_type, "evidence_source_type");
    out.provenance = read_enum<EvidenceProvenance>(reader, is_valid_evidence_provenance, "evidence_provenance");
    out.integrity = read_enum<EvidenceIntegrity>(reader, is_valid_evidence_integrity, "evidence_integrity");
    out.producer = read_id<ProducerId>(reader);
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.target_digest = reader.digest();
    out.review = read_id<ReviewId>(reader);
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.critic = read_id<CriticId>(reader);
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.worker = read_id<WorkerId>(reader);
    out.worker_boot = read_id<WorkerBootId>(reader);
    out.finding = read_id<FindingId>(reader);
    out.content_digest = reader.digest();
    out.payload = reader.bytes(limits.max_evidence_payload_bytes);
    out.reference = reader.string(limits.max_metadata_string_bytes);
    out.created_order = reader.u64();
    out.current = reader.boolean();
    out.superseded = reader.boolean();
    out.superseded_by = read_id<EvidenceId>(reader);
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.generation.valid() || !out.target.valid()) {
        return Status::error(reader.policy().malformed, "evidence record carried an invalid identity or binding");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const Finding& value) {
    write_id(writer, value.id);
    writer.u64(value.generation.value());
    write_id(writer, value.supersedes);
    write_id(writer, value.superseded_by);
    write_id(writer, value.critic);
    writer.u64(value.critic_generation.value());
    write_id(writer, value.assignment);
    write_id(writer, value.worker);
    write_id(writer, value.worker_boot);
    writer.u64(value.attempt.value());
    write_id(writer, value.review);
    writer.u64(value.review_generation.value());
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    write_enum(writer, value.category);
    write_enum(writer, value.severity);
    write_enum(writer, value.outcome);
    write_id_list(writer, value.evidence, kHardMaxFindingsPerReview);
    writer.string(value.explanation, kHardMaxPayloadBytes);
    encode(writer, value.confidence);
    writer.string(value.provenance, kHardMaxMetadataBytes);
    writer.u64(value.creation_order);
    write_enum(writer, value.state);
    write_id(writer, value.challenged_by);
    writer.digest(value.finding_digest);
}

Status decode(ByteReader& reader, Finding& out, const RuntimeLimits& limits) {
    out.id = read_id<FindingId>(reader);
    out.generation = FindingGeneration::from_value(reader.u64());
    out.supersedes = read_id<FindingId>(reader);
    out.superseded_by = read_id<FindingId>(reader);
    out.critic = read_id<CriticId>(reader);
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.assignment = read_id<CriticAssignmentId>(reader);
    out.worker = read_id<WorkerId>(reader);
    out.worker_boot = read_id<WorkerBootId>(reader);
    out.attempt = AttemptGeneration::from_value(reader.u64());
    out.review = read_id<ReviewId>(reader);
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.category = read_enum<FindingCategory>(reader, is_valid_finding_category, "finding_category");
    out.severity = read_enum<Severity>(reader, is_valid_severity, "severity");
    out.outcome = read_enum<FindingOutcome>(reader, is_valid_finding_outcome, "finding_outcome");
    Status status = read_id_list(reader, out.evidence, limits.max_evidence_per_finding);
    if (!status.ok()) {
        return status;
    }
    out.explanation = reader.string(limits.max_explanation_bytes);
    status = decode(reader, out.confidence);
    if (!status.ok()) {
        return status;
    }
    out.provenance = reader.string(limits.max_metadata_string_bytes);
    out.creation_order = reader.u64();
    out.state = read_enum<FindingState>(reader, is_valid_finding_state, "finding_state");
    out.challenged_by = read_id<ChallengeId>(reader);
    out.finding_digest = reader.digest();
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.generation.valid() || !out.critic.valid() || !out.critic_generation.valid() ||
        !out.review.valid() || !out.review_generation.valid() || !out.target.valid() ||
        !out.target_generation.valid()) {
        return Status::error(reader.policy().malformed, "finding carried an incomplete authority binding");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const Challenge& value) {
    write_id(writer, value.id);
    writer.u64(value.generation.value());
    write_id(writer, value.challenged_finding);
    writer.u64(value.challenged_finding_generation.value());
    write_id(writer, value.challenger);
    writer.u64(value.challenger_generation.value());
    write_id(writer, value.worker);
    write_id(writer, value.worker_boot);
    write_id(writer, value.review);
    writer.u64(value.review_generation.value());
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    writer.string(value.disputed_predicate, kHardMaxMetadataBytes);
    writer.string(value.rationale, kHardMaxPayloadBytes);
    write_id_list(writer, value.evidence, kHardMaxFindingsPerReview);
    writer.u64(value.creation_order);
    write_enum(writer, value.outcome);
    writer.boolean(value.resolved);
    write_id(writer, value.rebuttal);
}

Status decode(ByteReader& reader, Challenge& out, const RuntimeLimits& limits) {
    out.id = read_id<ChallengeId>(reader);
    out.generation = ChallengeGeneration::from_value(reader.u64());
    out.challenged_finding = read_id<FindingId>(reader);
    out.challenged_finding_generation = FindingGeneration::from_value(reader.u64());
    out.challenger = read_id<CriticId>(reader);
    out.challenger_generation = CriticGeneration::from_value(reader.u64());
    out.worker = read_id<WorkerId>(reader);
    out.worker_boot = read_id<WorkerBootId>(reader);
    out.review = read_id<ReviewId>(reader);
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.disputed_predicate = reader.string(limits.max_metadata_string_bytes);
    out.rationale = reader.string(limits.max_explanation_bytes);
    Status status = read_id_list(reader, out.evidence, limits.max_evidence_per_finding);
    if (!status.ok()) {
        return status;
    }
    out.creation_order = reader.u64();
    out.outcome = read_enum<ChallengeOutcome>(reader, is_valid_challenge_outcome, "challenge_outcome");
    out.resolved = reader.boolean();
    out.rebuttal = read_id<RebuttalId>(reader);
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.challenged_finding.valid() || !out.review.valid() || !out.target.valid()) {
        return Status::error(reader.policy().malformed, "challenge carried an incomplete authority binding");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const Rebuttal& value) {
    write_id(writer, value.id);
    write_id(writer, value.challenge);
    writer.u64(value.challenge_generation.value());
    write_id(writer, value.finding);
    write_id(writer, value.author);
    writer.u64(value.author_generation.value());
    write_id(writer, value.worker);
    write_id(writer, value.worker_boot);
    write_id(writer, value.review);
    writer.u64(value.review_generation.value());
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    writer.string(value.argument, kHardMaxPayloadBytes);
    write_id_list(writer, value.evidence, kHardMaxFindingsPerReview);
    writer.u64(value.creation_order);
}

Status decode(ByteReader& reader, Rebuttal& out, const RuntimeLimits& limits) {
    out.id = read_id<RebuttalId>(reader);
    out.challenge = read_id<ChallengeId>(reader);
    out.challenge_generation = ChallengeGeneration::from_value(reader.u64());
    out.finding = read_id<FindingId>(reader);
    out.author = read_id<CriticId>(reader);
    out.author_generation = CriticGeneration::from_value(reader.u64());
    out.worker = read_id<WorkerId>(reader);
    out.worker_boot = read_id<WorkerBootId>(reader);
    out.review = read_id<ReviewId>(reader);
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.argument = reader.string(limits.max_explanation_bytes);
    Status status = read_id_list(reader, out.evidence, limits.max_evidence_per_finding);
    if (!status.ok()) {
        return status;
    }
    out.creation_order = reader.u64();
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.challenge.valid()) {
        return Status::error(reader.policy().malformed, "rebuttal carried an incomplete authority binding");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const CriticAssignment& value) {
    write_id(writer, value.id);
    write_id(writer, value.review);
    writer.u64(value.review_generation.value());
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    write_id(writer, value.critic);
    writer.u64(value.critic_generation.value());
    write_id(writer, value.worker);
    write_id(writer, value.worker_boot);
    write_enum(writer, value.role);
    writer.boolean(value.required);
    writer.u32(value.weight);
    writer.u64(value.assigned_order);
    writer.boolean(value.active);
    writer.u64(value.attempt.value());
    writer.string(value.retirement_reason, kHardMaxMetadataBytes);
}

Status decode(ByteReader& reader, CriticAssignment& out, const RuntimeLimits& limits) {
    out.id = read_id<CriticAssignmentId>(reader);
    out.review = read_id<ReviewId>(reader);
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.critic = read_id<CriticId>(reader);
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.worker = read_id<WorkerId>(reader);
    out.worker_boot = read_id<WorkerBootId>(reader);
    out.role = read_enum<CriticRole>(reader, is_valid_critic_role, "critic_role");
    out.required = reader.boolean();
    out.weight = reader.u32();
    out.assigned_order = reader.u64();
    out.active = reader.boolean();
    out.attempt = AttemptGeneration::from_value(reader.u64());
    out.retirement_reason = reader.string(limits.max_metadata_string_bytes);
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.review.valid() || !out.critic.valid()) {
        return Status::error(reader.policy().malformed, "assignment carried an incomplete authority binding");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const PredicateSpec& value) {
    write_enum(writer, value.kind);
    writer.boolean(value.mandatory);
    writer.u32(value.parameter);
}

void encode(ByteWriter& writer, const PolicyTableEntry& value) {
    write_enum(writer, value.outcome);
    write_enum(writer, value.min_severity);
    write_enum(writer, value.max_severity);
    writer.u32(value.min_count);
    write_enum(writer, value.disposition);
}

void encode(ByteWriter& writer, const ReviewSpec& value) {
    writer.u32(kSpecVersion);
    writer.string(value.name, kHardMaxMetadataBytes);
    write_role_list(writer, value.required_roles, kCriticRoleMax);
    write_role_list(writer, value.optional_roles, kCriticRoleMax);
    write_roles(writer, value.roles_that_must_pass, kCriticRoleMax);
    writer.u32(value.min_critic_count);
    writer.u32(value.required_critic_count);
    writer.u32(value.quorum);
    write_item_count(writer, value.required_min_categories.size(), kFindingCategoryMax, "required category");
    for (const auto& entry : value.required_min_categories) {
        write_enum(writer, entry.first);
        writer.u32(entry.second);
    }
    write_item_count(writer, value.required_finding_categories.size(), kFindingCategoryMax, "required category");
    for (FindingCategory category : value.required_finding_categories) {
        write_enum(writer, category);
    }
    writer.u32(value.max_critical_fails);
    writer.boolean(value.allow_abstention);
    writer.boolean(value.allow_partial_failure);
    write_enum(writer, value.fail_threshold);
    write_enum(writer, value.warn_threshold);
    writer.boolean(value.warn_counts_as_failure);
    writer.boolean(value.unknown_counts_as_failure);
    write_enum(writer, value.aggregation);
    write_enum(writer, value.disagreement);
    writer.u32(value.disagreement_threshold_fail);
    write_enum(writer, value.duplicates);
    write_enum(writer, value.challenges);
    writer.u32(value.max_challenge_rounds);
    writer.u32(value.max_review_rounds);
    writer.u32(value.max_critic_retries);
    write_item_count(writer, value.critic_weights.size(), kHardMaxCriticsPerReview, "critic weight");
    for (const auto& entry : value.critic_weights) {
        writer.u64(entry.first.value());
        writer.u32(entry.second);
    }
    writer.u32(value.default_weight);
    write_item_count(writer, value.predicates.size(), 64, "predicate");
    for (const PredicateSpec& predicate : value.predicates) {
        encode(writer, predicate);
    }
    write_item_count(writer, value.policy_table.size(), 64, "policy table row");
    for (const PolicyTableEntry& entry : value.policy_table) {
        encode(writer, entry);
    }
    writer.u32(value.consensus_threshold_basis_points);
    writer.boolean(value.confidence_has_authority);
    writer.boolean(value.deterministic_decision_required);
    writer.boolean(value.persist_findings);
    writer.boolean(value.persist_evidence_metadata);
    writer.boolean(value.persist_evidence_payload);
    writer.boolean(value.reopen_on_recovery);
}

Status decode(ByteReader& reader, ReviewSpec& out, const RuntimeLimits& limits) {
    const std::uint32_t version = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (version != kSpecVersion) {
        return Status::error(reader.policy().malformed, "unsupported review specification encoding version");
    }
    out.name = reader.string(limits.max_metadata_string_bytes);
    Status status = read_role_list(reader, out.required_roles, kCriticRoleMax);
    if (!status.ok()) {
        return status;
    }
    status = read_role_list(reader, out.optional_roles, kCriticRoleMax);
    if (!status.ok()) {
        return status;
    }
    status = read_roles(reader, out.roles_that_must_pass, kCriticRoleMax);
    if (!status.ok()) {
        return status;
    }
    out.min_critic_count = reader.u32();
    out.required_critic_count = reader.u32();
    out.quorum = reader.u32();
    std::uint32_t count = 0;
    if (!begin_items(reader, kFindingCategoryMax, "required category", count)) {
        return reader.status();
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const FindingCategory category = read_enum<FindingCategory>(reader, is_valid_finding_category, "finding_category");
        const std::uint32_t minimum = reader.u32();
        out.required_min_categories[category] = minimum;
    }
    if (!begin_items(reader, kFindingCategoryMax, "required category", count)) {
        return reader.status();
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        out.required_finding_categories.push_back(
            read_enum<FindingCategory>(reader, is_valid_finding_category, "finding_category"));
    }
    out.max_critical_fails = reader.u32();
    out.allow_abstention = reader.boolean();
    out.allow_partial_failure = reader.boolean();
    out.fail_threshold = read_enum<Severity>(reader, is_valid_severity, "severity");
    out.warn_threshold = read_enum<Severity>(reader, is_valid_severity, "severity");
    out.warn_counts_as_failure = reader.boolean();
    out.unknown_counts_as_failure = reader.boolean();
    out.aggregation = read_enum<AggregationPolicy>(reader, is_valid_aggregation_policy, "aggregation_policy");
    out.disagreement = read_enum<DisagreementPolicy>(reader, is_valid_disagreement_policy, "disagreement_policy");
    out.disagreement_threshold_fail = reader.u32();
    out.duplicates = read_enum<DuplicatePolicy>(reader, is_valid_duplicate_policy, "duplicate_policy");
    out.challenges = read_enum<ChallengePolicy>(reader, is_valid_challenge_policy, "challenge_policy");
    out.max_challenge_rounds = reader.u32();
    out.max_review_rounds = reader.u32();
    out.max_critic_retries = reader.u32();
    if (!begin_items(reader, kHardMaxCriticsPerReview, "critic weight", count)) {
        return reader.status();
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const CriticId critic = CriticId::from_value(reader.u64());
        out.critic_weights[critic] = reader.u32();
    }
    out.default_weight = reader.u32();
    if (!begin_items(reader, 64, "predicate", count)) {
        return reader.status();
    }
    out.predicates.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PredicateSpec predicate;
        predicate.kind = read_enum<PredicateKind>(reader, is_valid_predicate_kind, "predicate_kind");
        predicate.mandatory = reader.boolean();
        predicate.parameter = reader.u32();
        out.predicates.push_back(predicate);
    }
    if (!begin_items(reader, 64, "policy table row", count)) {
        return reader.status();
    }
    out.policy_table.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PolicyTableEntry entry;
        entry.outcome = read_enum<FindingOutcome>(reader, is_valid_finding_outcome, "finding_outcome");
        entry.min_severity = read_enum<Severity>(reader, is_valid_severity, "severity");
        entry.max_severity = read_enum<Severity>(reader, is_valid_severity, "severity");
        entry.min_count = reader.u32();
        entry.disposition = read_enum<ReviewDisposition>(reader, is_valid_review_disposition, "review_disposition");
        out.policy_table.push_back(entry);
    }
    out.consensus_threshold_basis_points = reader.u32();
    out.confidence_has_authority = reader.boolean();
    out.deterministic_decision_required = reader.boolean();
    out.persist_findings = reader.boolean();
    out.persist_evidence_metadata = reader.boolean();
    out.persist_evidence_payload = reader.boolean();
    out.reopen_on_recovery = reader.boolean();
    if (!reader.ok()) {
        return reader.status();
    }
    if (out.confidence_has_authority) {
        return Status::error(ErrorCode::PolicyRejected,
                             "confidence may not be granted decision authority; it is advisory metadata only");
    }
    if (out.consensus_threshold_basis_points > 10000u) {
        return Status::error(reader.policy().malformed, "consensus threshold outside 0..10000 basis points");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const ReviewRecord& value) {
    write_id(writer, value.id);
    write_id(writer, value.request);
    writer.u64(value.generation.value());
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    writer.digest(value.target_digest);
    encode(writer, value.spec);
    write_enum(writer, value.state);
    write_enum(writer, value.disposition);
    writer.u64(value.declared_order);
    writer.u64(value.updated_order);
    writer.u64(value.superseded_by.value());
    writer.string(value.cancellation_reason, kHardMaxMetadataBytes);
    writer.string(value.status_detail, kHardMaxMetadataBytes);
    writer.u32(value.rounds_used);
    writer.boolean(value.authority_current);
}

Status decode(ByteReader& reader, ReviewRecord& out, const RuntimeLimits& limits) {
    out.id = read_id<ReviewId>(reader);
    out.request = read_id<ReviewRequestId>(reader);
    out.generation = ReviewGeneration::from_value(reader.u64());
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.target_digest = reader.digest();
    Status status = decode(reader, out.spec, limits);
    if (!status.ok()) {
        return status;
    }
    out.state = read_enum<ReviewState>(reader, is_valid_review_state, "review_state");
    out.disposition = read_enum<ReviewDisposition>(reader, is_valid_review_disposition, "review_disposition");
    out.declared_order = reader.u64();
    out.updated_order = reader.u64();
    out.superseded_by = ReviewGeneration::from_value(reader.u64());
    out.cancellation_reason = reader.string(limits.max_metadata_string_bytes);
    out.status_detail = reader.string(limits.max_metadata_string_bytes);
    out.rounds_used = reader.u32();
    out.authority_current = reader.boolean();
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.generation.valid() || !out.target.valid()) {
        return Status::error(reader.policy().malformed, "review record carried an invalid identity or generation");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const Decision& value) {
    write_id(writer, value.id);
    writer.u64(value.generation.value());
    write_id(writer, value.review);
    writer.u64(value.review_generation.value());
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    write_enum(writer, value.disposition);
    writer.digest(value.decision_digest);
    writer.digest(value.policy_digest);
    writer.digest(value.evidence_basis_digest);
    writer.u64(value.committed_order);
    writer.u64(value.epoch.value());
}

Status decode(ByteReader& reader, Decision& out) {
    out.id = read_id<DecisionId>(reader);
    out.generation = DecisionGeneration::from_value(reader.u64());
    out.review = read_id<ReviewId>(reader);
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.disposition = read_enum<ReviewDisposition>(reader, is_valid_review_disposition, "review_disposition");
    out.decision_digest = reader.digest();
    out.policy_digest = reader.digest();
    out.evidence_basis_digest = reader.digest();
    out.committed_order = reader.u64();
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    if (!reader.ok()) {
        return reader.status();
    }
    if (!out.id.valid() || !out.review.valid() || !out.generation.valid()) {
        return Status::error(reader.policy().malformed, "decision carried an invalid identity or generation");
    }
    return Status::success();
}

void encode(ByteWriter& writer, const AuthorityContext& value) {
    writer.u64(value.epoch.value());
    write_id(writer, value.worker);
    write_id(writer, value.worker_boot);
    write_id(writer, value.critic);
    writer.u64(value.critic_generation.value());
    write_id(writer, value.review);
    writer.u64(value.review_generation.value());
    write_id(writer, value.target);
    writer.u64(value.target_generation.value());
    write_id(writer, value.request);
}

Status decode(ByteReader& reader, AuthorityContext& out) {
    out.epoch = CoordinatorEpoch::from_value(reader.u64());
    out.worker = read_id<WorkerId>(reader);
    out.worker_boot = read_id<WorkerBootId>(reader);
    out.critic = read_id<CriticId>(reader);
    out.critic_generation = CriticGeneration::from_value(reader.u64());
    out.review = read_id<ReviewId>(reader);
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.target = read_id<TargetId>(reader);
    out.target_generation = TargetGeneration::from_value(reader.u64());
    out.request = read_id<RequestId>(reader);
    return reader.status();
}

}  // namespace critic_fabric

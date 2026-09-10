// Critic Fabric — canonical, checked byte encoding.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_SERIALIZATION_HPP
#define CRITIC_FABRIC_SERIALIZATION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "critic_fabric/authority.hpp"
#include "critic_fabric/challenge.hpp"
#include "critic_fabric/critic.hpp"
#include "critic_fabric/digest.hpp"
#include "critic_fabric/evidence.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/ids.hpp"
#include "critic_fabric/limits.hpp"
#include "critic_fabric/policy.hpp"
#include "critic_fabric/result.hpp"
#include "critic_fabric/review.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

// Which typed code a reader reports for each class of decoding fault. The same
// codec serves the wire protocol and the persistence journal, which must not
// report transport errors for durable-state damage.
struct ReaderPolicy {
    ErrorCode truncated = ErrorCode::ProtocolTruncated;
    ErrorCode oversized = ErrorCode::ProtocolOversized;
    ErrorCode invalid_enum = ErrorCode::ProtocolInvalidEnum;
    ErrorCode malformed = ErrorCode::ProtocolError;
    ErrorCode overflow = ErrorCode::ArithmeticOverflow;
};

// Fixed-width, big-endian writer. Sizes are checked before allocation; every
// length field is bounded by an explicit caller-supplied maximum.
class ByteWriter {
public:
    ByteWriter() = default;

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void boolean(bool value);
    void raw(const std::uint8_t* data, std::size_t len);
    void digest(const Digest& value);
    void string(std::string_view value, std::uint32_t max_bytes);
    void bytes(const std::vector<std::uint8_t>& value, std::uint32_t max_bytes);

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

    [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buffer_); }

    void reserve(std::size_t bytes);
    void fail(ErrorCode code, std::string detail);
    // Overwrites a previously written big-endian u32 in place.
    void patch_u32(std::size_t offset, std::uint32_t value);

    [[nodiscard]] Status status() const;

private:
    void require(std::size_t additional);

    std::vector<std::uint8_t> buffer_;
    bool ok_ = true;
    ErrorCode code_ = ErrorCode::Ok;
    std::string detail_;
};

// Fixed-width, big-endian reader with a sticky error. Once a fault is recorded
// every subsequent read is a no-op returning a zero value, so callers may check
// ok() once at the end without risking an out-of-bounds access.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t len, ReaderPolicy policy = {});
    explicit ByteReader(const std::vector<std::uint8_t>& data, ReaderPolicy policy = {});

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int64_t i64();
    bool boolean();
    Digest digest();
    std::string string(std::uint32_t max_bytes);
    std::vector<std::uint8_t> bytes(std::uint32_t max_bytes);

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return len_ - pos_; }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ == len_; }
    [[nodiscard]] Status status() const;
    [[nodiscard]] const ReaderPolicy& policy() const noexcept { return policy_; }

    void fail(ErrorCode code, std::string detail);
    bool skip(std::size_t count);

private:
    bool need(std::size_t count);
    [[nodiscard]] const std::uint8_t* cursor() const noexcept { return data_ + pos_; }

    const std::uint8_t* data_ = nullptr;
    std::size_t len_ = 0;
    std::size_t pos_ = 0;
    bool ok_ = true;
    ErrorCode code_ = ErrorCode::Ok;
    std::string detail_;
    ReaderPolicy policy_;
};

template <typename E>
void write_enum(ByteWriter& writer, E value) {
    writer.u32(static_cast<std::uint32_t>(value));
}

template <typename E>
E read_enum(ByteReader& reader, bool (*validator)(std::uint32_t), const char* field_name) {
    const std::uint32_t raw = reader.u32();
    if (reader.ok() && !validator(raw)) {
        reader.fail(reader.policy().invalid_enum, std::string("invalid enumeration value for ") + field_name);
    }
    return static_cast<E>(raw);
}

template <typename Id>
void write_id(ByteWriter& writer, Id value) {
    writer.u64(value.value());
}

template <typename Id>
Id read_id(ByteReader& reader) {
    return Id::from_value(reader.u64());
}

// --- Domain codecs -----------------------------------------------------------
// Each decoder is total: it either yields a fully validated value or records a
// typed fault on the reader. It never leaves a partially initialised object
// that a caller could mistake for valid.

void encode(ByteWriter& writer, const Digest& value);
void encode(ByteWriter& writer, const Compatibility& value);
void encode(ByteWriter& writer, const TargetRecord& value);
void encode(ByteWriter& writer, const CriticCapability& value);
void encode(ByteWriter& writer, const CriticRegistration& value);
void encode(ByteWriter& writer, const WorkerRecord& value);
void encode(ByteWriter& writer, const Confidence& value);
void encode(ByteWriter& writer, const EvidenceRecord& value);
void encode(ByteWriter& writer, const Finding& value);
void encode(ByteWriter& writer, const Challenge& value);
void encode(ByteWriter& writer, const Rebuttal& value);
void encode(ByteWriter& writer, const CriticAssignment& value);
void encode(ByteWriter& writer, const PredicateSpec& value);
void encode(ByteWriter& writer, const PolicyTableEntry& value);
void encode(ByteWriter& writer, const ReviewSpec& value);
void encode(ByteWriter& writer, const ReviewRecord& value);
void encode(ByteWriter& writer, const Decision& value);
void encode(ByteWriter& writer, const AuthorityContext& value);

Status decode(ByteReader& reader, Compatibility& out);
Status decode(ByteReader& reader, Confidence& out);
Status decode(ByteReader& reader, TargetRecord& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, CriticCapability& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, CriticRegistration& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, WorkerRecord& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, EvidenceRecord& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, Finding& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, Challenge& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, Rebuttal& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, CriticAssignment& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, ReviewSpec& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, ReviewRecord& out, const RuntimeLimits& limits);
Status decode(ByteReader& reader, Decision& out);
Status decode(ByteReader& reader, AuthorityContext& out);

// Canonical digest over an encoded value. Two structurally equal values always
// produce the same digest; encoding order is fixed by the encoder, not by any
// unordered container iteration.
template <typename T, typename EncodeFn>
Digest digest_of(const T& value, EncodeFn encode_fn) {
    ByteWriter writer;
    encode_fn(writer, value);
    return Sha256::hash(writer.buffer());
}

// Bounds used by the wire protocol and the persistence journal.
struct CodecLimits {
    std::uint32_t max_string_bytes = 1024;
    std::uint32_t max_payload_bytes = 65536;
    std::uint32_t max_items = 4096;
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_SERIALIZATION_HPP

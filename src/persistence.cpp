// Critic Fabric — durable journal implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/persistence.hpp"

#include <cstring>
#include <utility>

#include "critic_fabric/serialization.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace critic_fabric {
namespace {

constexpr char kJournalMagic[8] = {'C', 'F', 'A', 'B', 'J', 'R', 'N', 'L'};
constexpr char kTrailerMagic[8] = {'C', 'F', 'A', 'B', 'E', 'N', 'D', '!'};
constexpr std::uint32_t kJournalFormatVersion = 1;
constexpr std::uint32_t kHardMaxJournalRecordBytes = 8u * 1024u * 1024u;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t get_u32(const std::uint8_t* p) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<std::uint32_t>(p[i]);
    }
    return value;
}

std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return value;
}

void append_bytes(std::vector<std::uint8_t>& out, const std::uint8_t* data, std::size_t len) {
    out.insert(out.end(), data, data + len);
}

Status read_entire_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, path.c_str(), "rb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
        return Status::error(ErrorCode::PersistenceIoError, "unable to open journal for reading");
    }
    std::uint8_t buffer[65536];
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        if (got > 0) {
            out.insert(out.end(), buffer, buffer + got);
        }
        if (got < sizeof(buffer)) {
            if (std::ferror(file) != 0) {
                std::fclose(file);
                return Status::error(ErrorCode::PersistenceIoError, "journal read failed");
            }
            break;
        }
    }
    std::fclose(file);
    return Status::success();
}

bool file_exists(const std::string& path) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, path.c_str(), "rb") != 0) {
        return false;
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

Status atomic_replace(const std::string& source, const std::string& destination) {
#ifdef _WIN32
    const int source_len = MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, nullptr, 0);
    const int destination_len = MultiByteToWideChar(CP_UTF8, 0, destination.c_str(), -1, nullptr, 0);
    if (source_len <= 1 || destination_len <= 1) {
        return Status::error(ErrorCode::PersistenceIoError, "journal path is not valid UTF-8");
    }
    std::wstring wide_source(static_cast<std::size_t>(source_len - 1), L'\0');
    std::wstring wide_destination(static_cast<std::size_t>(destination_len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, wide_source.data(), source_len);
    MultiByteToWideChar(CP_UTF8, 0, destination.c_str(), -1, wide_destination.data(), destination_len);
    if (MoveFileExW(wide_source.c_str(), wide_destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return Status::error(ErrorCode::PersistenceIoError, "atomic journal replacement failed");
    }
    return Status::success();
#else
    if (std::rename(source.c_str(), destination.c_str()) != 0) {
        return Status::error(ErrorCode::PersistenceIoError, "atomic journal replacement failed");
    }
    return Status::success();
#endif
}

Status write_file_sync(const std::string& path, const std::vector<std::uint8_t>& content) {
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, path.c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), "wb");
#endif
    if (file == nullptr) {
        return Status::error(ErrorCode::PersistenceIoError, "unable to open journal for writing");
    }
    if (!content.empty() && std::fwrite(content.data(), 1, content.size(), file) != content.size()) {
        std::fclose(file);
        return Status::error(ErrorCode::PersistenceIoError, "journal write failed");
    }
    if (std::fflush(file) != 0) {
        std::fclose(file);
        return Status::error(ErrorCode::PersistenceIoError, "journal flush failed");
    }
#ifdef _WIN32
    _commit(_fileno(file));
#endif
    std::fclose(file);
    return Status::success();
}

ReaderPolicy journal_reader_policy() {
    ReaderPolicy policy;
    policy.truncated = ErrorCode::PersistenceTruncated;
    policy.oversized = ErrorCode::PersistenceCorruption;
    policy.invalid_enum = ErrorCode::PersistenceCorruption;
    policy.malformed = ErrorCode::PersistenceCorruption;
    policy.overflow = ErrorCode::PersistenceCorruption;
    return policy;
}

}  // namespace

const char* to_string(JournalRecordType type) noexcept {
    switch (type) {
        case JournalRecordType::Unspecified: return "UNSPECIFIED";
        case JournalRecordType::EpochEstablished: return "EPOCH_ESTABLISHED";
        case JournalRecordType::TargetRegistered: return "TARGET_REGISTERED";
        case JournalRecordType::TargetSuperseded: return "TARGET_SUPERSEDED";
        case JournalRecordType::CriticRegistered: return "CRITIC_REGISTERED";
        case JournalRecordType::WorkerRegistered: return "WORKER_REGISTERED";
        case JournalRecordType::ReviewDeclared: return "REVIEW_DECLARED";
        case JournalRecordType::AssignmentsAdded: return "ASSIGNMENTS_ADDED";
        case JournalRecordType::FindingRecorded: return "FINDING_RECORDED";
        case JournalRecordType::FindingStateChanged: return "FINDING_STATE_CHANGED";
        case JournalRecordType::EvidenceRecorded: return "EVIDENCE_RECORDED";
        case JournalRecordType::ChallengeRecorded: return "CHALLENGE_RECORDED";
        case JournalRecordType::RebuttalRecorded: return "REBUTTAL_RECORDED";
        case JournalRecordType::ReviewStateChanged: return "REVIEW_STATE_CHANGED";
        case JournalRecordType::DecisionCommitted: return "DECISION_COMMITTED";
        case JournalRecordType::AssignmentUpdated: return "ASSIGNMENT_UPDATED";
    }
    return "INVALID";
}

bool is_valid_journal_record_type(std::uint16_t raw) noexcept {
    return raw >= 1 && raw <= kJournalRecordTypeMax;
}

void encode(ByteWriter& writer, const FindingStateChange& value) {
    writer.u64(value.finding.value());
    writer.u32(static_cast<std::uint32_t>(value.state));
    writer.u64(value.superseded_by.value());
    writer.u64(value.challenged_by.value());
    writer.string(value.reason, kHardMaxMetadataBytes);
}

Status decode(ByteReader& reader, FindingStateChange& out, const RuntimeLimits& limits) {
    out.finding = FindingId::from_value(reader.u64());
    out.state = read_enum<FindingState>(reader, is_valid_finding_state, "finding_state");
    out.superseded_by = FindingId::from_value(reader.u64());
    out.challenged_by = ChallengeId::from_value(reader.u64());
    out.reason = reader.string(limits.max_metadata_string_bytes);
    return reader.status();
}

void encode(ByteWriter& writer, const ReviewStateChange& value) {
    writer.u64(value.review.value());
    writer.u64(value.review_generation.value());
    writer.u32(static_cast<std::uint32_t>(value.state));
    writer.u32(static_cast<std::uint32_t>(value.disposition));
    writer.u64(value.superseded_by.value());
    writer.string(value.detail, kHardMaxMetadataBytes);
    writer.string(value.cancellation_reason, kHardMaxMetadataBytes);
    writer.boolean(value.authority_current);
    writer.u32(value.rounds_used);
}

Status decode(ByteReader& reader, ReviewStateChange& out, const RuntimeLimits& limits) {
    out.review = ReviewId::from_value(reader.u64());
    out.review_generation = ReviewGeneration::from_value(reader.u64());
    out.state = read_enum<ReviewState>(reader, is_valid_review_state, "review_state");
    out.disposition = read_enum<ReviewDisposition>(reader, is_valid_review_disposition, "review_disposition");
    out.superseded_by = ReviewGeneration::from_value(reader.u64());
    out.detail = reader.string(limits.max_metadata_string_bytes);
    out.cancellation_reason = reader.string(limits.max_metadata_string_bytes);
    out.authority_current = reader.boolean();
    out.rounds_used = reader.u32();
    return reader.status();
}

void encode(ByteWriter& writer, const JournalEntry& value) {
    writer.u16(static_cast<std::uint16_t>(value.type));
    switch (value.type) {
        case JournalRecordType::EpochEstablished:
            writer.u64(value.epoch.value());
            break;
        case JournalRecordType::TargetRegistered:
            encode(writer, value.target);
            break;
        case JournalRecordType::TargetSuperseded:
            writer.u64(value.target.id.value());
            writer.u64(value.previous_target_generation.value());
            encode(writer, value.previous_target);
            encode(writer, value.target);
            break;
        case JournalRecordType::CriticRegistered:
            encode(writer, value.critic);
            break;
        case JournalRecordType::WorkerRegistered:
            encode(writer, value.worker);
            break;
        case JournalRecordType::ReviewDeclared:
            encode(writer, value.review);
            break;
        case JournalRecordType::AssignmentsAdded:
            writer.u64(value.review.id.value());
            writer.u64(value.review.generation.value());
            writer.u32(static_cast<std::uint32_t>(value.assignments.size()));
            for (const CriticAssignment& assignment : value.assignments) {
                encode(writer, assignment);
            }
            break;
        case JournalRecordType::FindingRecorded:
            encode(writer, value.finding);
            break;
        case JournalRecordType::FindingStateChanged:
            encode(writer, value.finding_state);
            break;
        case JournalRecordType::EvidenceRecorded:
            encode(writer, value.evidence);
            break;
        case JournalRecordType::ChallengeRecorded:
            encode(writer, value.challenge);
            break;
        case JournalRecordType::RebuttalRecorded:
            encode(writer, value.rebuttal);
            break;
        case JournalRecordType::ReviewStateChanged:
            encode(writer, value.review_state);
            break;
        case JournalRecordType::DecisionCommitted:
            encode(writer, value.decision);
            break;
        case JournalRecordType::AssignmentUpdated:
            encode(writer, value.assignment);
            break;
        case JournalRecordType::Unspecified:
            break;
    }
}

Status decode(ByteReader& reader, JournalEntry& out, const RuntimeLimits& limits) {
    const std::uint16_t raw_type = reader.u16();
    if (!reader.ok()) {
        return reader.status();
    }
    if (!is_valid_journal_record_type(raw_type)) {
        return Status::error(ErrorCode::PersistenceCorruption, "journal record carries an unknown record type");
    }
    out.type = static_cast<JournalRecordType>(raw_type);
    Status status = Status::success();
    switch (out.type) {
        case JournalRecordType::EpochEstablished:
            out.epoch = CoordinatorEpoch::from_value(reader.u64());
            break;
        case JournalRecordType::TargetRegistered:
            status = decode(reader, out.target, limits);
            break;
        case JournalRecordType::TargetSuperseded:
            out.target.id = TargetId::from_value(reader.u64());
            out.previous_target_generation = TargetGeneration::from_value(reader.u64());
            status = decode(reader, out.previous_target, limits);
            if (!status.ok()) {
                return status;
            }
            status = decode(reader, out.target, limits);
            break;
        case JournalRecordType::CriticRegistered:
            status = decode(reader, out.critic, limits);
            break;
        case JournalRecordType::WorkerRegistered:
            status = decode(reader, out.worker, limits);
            break;
        case JournalRecordType::ReviewDeclared:
            status = decode(reader, out.review, limits);
            break;
        case JournalRecordType::AssignmentsAdded: {
            out.review.id = ReviewId::from_value(reader.u64());
            out.review.generation = ReviewGeneration::from_value(reader.u64());
            const std::uint32_t count = reader.u32();
            if (!reader.ok()) {
                return reader.status();
            }
            if (count > limits.max_critics_per_review) {
                return Status::error(ErrorCode::PersistenceCorruption,
                                     "assignment batch exceeds the configured critic bound");
            }
            out.assignments.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                CriticAssignment assignment;
                status = decode(reader, assignment, limits);
                if (!status.ok()) {
                    return status;
                }
                out.assignments.push_back(std::move(assignment));
            }
            break;
        }
        case JournalRecordType::FindingRecorded:
            status = decode(reader, out.finding, limits);
            break;
        case JournalRecordType::FindingStateChanged:
            status = decode(reader, out.finding_state, limits);
            break;
        case JournalRecordType::EvidenceRecorded:
            status = decode(reader, out.evidence, limits);
            break;
        case JournalRecordType::ChallengeRecorded:
            status = decode(reader, out.challenge, limits);
            break;
        case JournalRecordType::RebuttalRecorded:
            status = decode(reader, out.rebuttal, limits);
            break;
        case JournalRecordType::ReviewStateChanged:
            status = decode(reader, out.review_state, limits);
            break;
        case JournalRecordType::DecisionCommitted:
            status = decode(reader, out.decision);
            break;
        case JournalRecordType::AssignmentUpdated:
            status = decode(reader, out.assignment, limits);
            break;
        case JournalRecordType::Unspecified:
            return Status::error(ErrorCode::PersistenceCorruption, "journal record type is unspecified");
    }
    if (!status.ok()) {
        return status;
    }
    return reader.status();
}

// --- PersistentJournal -------------------------------------------------------

PersistentJournal::~PersistentJournal() { close(); }

Status PersistentJournal::open(const std::string& path, const RuntimeLimits& limits) {
    limits_ = limits;
    limits_.clamp_to_hard_ceiling();
    path_ = path;
    entries_.clear();
    next_sequence_ = 1;
    running_crc_ = 0;
    failed_ = false;

    if (!file_exists(path_)) {
        std::vector<std::uint8_t> content;
        content.reserve(kHeaderBytes + kTrailerBytes);
        append_bytes(content, reinterpret_cast<const std::uint8_t*>(kJournalMagic), sizeof(kJournalMagic));
        put_u32(content, kJournalFormatVersion);
        put_u32(content, 0);
        const std::uint32_t header_crc = crc32(content.data(), content.size());
        append_bytes(content, reinterpret_cast<const std::uint8_t*>(kTrailerMagic), sizeof(kTrailerMagic));
        put_u64(content, 0);
        put_u32(content, header_crc);
        put_u32(content, 0);
        Status status = write_file_sync(path_, content);
        if (!status.ok()) {
            return status;
        }
        valid_end_offset_ = kHeaderBytes;
    }

    std::vector<std::uint8_t> raw;
    Status status = read_entire_file(path_, raw);
    if (!status.ok()) {
        return status;
    }
    if (raw.size() > limits_.max_persistence_file_bytes) {
        return Status::error(ErrorCode::ResourceLimitExceeded, "journal exceeds the configured file bound");
    }
    if (raw.size() < kHeaderBytes + kTrailerBytes) {
        return Status::error(ErrorCode::PersistenceTruncated,
                             "journal is smaller than a header plus an end marker");
    }
    if (std::memcmp(raw.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
        return Status::error(ErrorCode::PersistenceBadMagic, "journal header magic does not match");
    }
    const std::uint32_t version = get_u32(raw.data() + 8);
    if (version != kJournalFormatVersion) {
        return Status::error(ErrorCode::PersistenceUnsupportedVersion,
                             "journal format version is not supported by this runtime");
    }

    const std::size_t trailer_offset = raw.size() - kTrailerBytes;
    const std::uint8_t* trailer = raw.data() + trailer_offset;
    if (std::memcmp(trailer, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
        return Status::error(ErrorCode::PersistenceTrailingData,
                             "journal has no end marker at its end; the tail is either truncated, extended, "
                             "or damaged");
    }
    const std::uint64_t expected_records = get_u64(trailer + 8);
    const std::uint32_t expected_crc = get_u32(trailer + 16);
    const std::uint32_t actual_crc = crc32(raw.data(), trailer_offset);
    if (expected_crc != actual_crc) {
        return Status::error(ErrorCode::PersistenceChecksumMismatch,
                             "journal content checksum does not match the end marker");
    }

    const RuntimeLimits& limits_ref = limits_;
    ByteReader reader(raw.data() + kHeaderBytes, trailer_offset - kHeaderBytes, journal_reader_policy());
    std::uint64_t last_sequence = 0;
    std::uint64_t count = 0;
    while (!reader.at_end()) {
        if (reader.remaining() < kRecordHeaderBytes) {
            return Status::error(ErrorCode::PersistenceTruncated,
                                 "journal ends inside a record header");
        }
        const std::uint8_t* header = raw.data() + kHeaderBytes + reader.position();
        const std::uint16_t type_raw = get_u16(header);
        const std::uint16_t flags = get_u16(header + 2);
        const std::uint32_t payload_len = get_u32(header + 4);
        const std::uint64_t sequence = get_u64(header + 8);
        const std::uint32_t payload_crc = get_u32(header + 16);
        if (flags != 0) {
            return Status::error(ErrorCode::PersistenceCorruption, "journal record carries unknown flags");
        }
        if (payload_len > kHardMaxJournalRecordBytes) {
            return Status::error(ErrorCode::PersistenceCorruption,
                                 "journal record payload exceeds the hard record bound");
        }
        if (reader.remaining() < kRecordHeaderBytes + payload_len) {
            return Status::error(ErrorCode::PersistenceTruncated,
                                 "journal ends inside a record payload");
        }
        if (!is_valid_journal_record_type(type_raw)) {
            return Status::error(ErrorCode::PersistenceCorruption,
                                 "journal record carries an unknown record type");
        }
        const std::uint8_t* payload = header + kRecordHeaderBytes;
        if (crc32(payload, payload_len) != payload_crc) {
            return Status::error(ErrorCode::PersistenceChecksumMismatch,
                                 "journal record payload checksum does not match");
        }
        if (sequence <= last_sequence) {
            return Status::error(ErrorCode::PersistenceCorruption,
                                 "journal record sequence numbers are not strictly increasing");
        }
        last_sequence = sequence;
        if (count >= limits_ref.max_persisted_records) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "journal holds more records than the configured bound");
        }

        ByteReader record_reader(payload, payload_len, journal_reader_policy());
        JournalEntry entry;
        Status entry_status = decode(record_reader, entry, limits_ref);
        if (!entry_status.ok()) {
            return entry_status;
        }
        if (!record_reader.at_end()) {
            return Status::error(ErrorCode::PersistenceTrailingData,
                                 "journal record payload carries trailing bytes after its declared fields");
        }
        entry.sequence = sequence;
        entries_.push_back(std::move(entry));
        count += 1;
        reader.skip(kRecordHeaderBytes + payload_len);
    }

    if (count != expected_records) {
        return Status::error(ErrorCode::PersistenceCorruption,
                             "journal end marker record count does not match the records present");
    }

    valid_end_offset_ = trailer_offset;
    next_sequence_ = last_sequence + 1;
    running_crc_ = actual_crc;

#ifdef _WIN32
    if (fopen_s(&file_, path_.c_str(), "r+b") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(path_.c_str(), "r+b");
#endif
    if (file_ == nullptr) {
        return Status::error(ErrorCode::PersistenceIoError, "unable to reopen the journal for appending");
    }
    return Status::success();
}

void PersistentJournal::open_in_memory(const RuntimeLimits& limits) {
    limits_ = limits;
    limits_.clamp_to_hard_ceiling();
    path_.clear();
    file_ = nullptr;
    entries_.clear();
    next_sequence_ = 1;
    running_crc_ = 0;
    valid_end_offset_ = 0;
    failed_ = false;
}

Status PersistentJournal::write_bytes(const std::uint8_t* data, std::size_t len) {
    if (file_ == nullptr) {
        return Status::success();
    }
    if (len == 0) {
        return Status::success();
    }
    if (std::fwrite(data, 1, len, file_) != len) {
        return Status::error(ErrorCode::PersistenceIoError, "journal write failed");
    }
    return Status::success();
}

Status PersistentJournal::flush() {
    if (file_ == nullptr) {
        return Status::success();
    }
    if (std::fflush(file_) != 0) {
        return Status::error(ErrorCode::PersistenceIoError, "journal flush failed");
    }
#ifdef _WIN32
    if (_commit(_fileno(file_)) != 0) {
        return Status::error(ErrorCode::PersistenceIoError, "journal durable flush failed");
    }
#endif
    return Status::success();
}

Status PersistentJournal::append(const JournalEntry& entry) {
    if (failed_) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "journal has entered a failed state and rejects further records");
    }
    if (entries_.size() >= limits_.max_persisted_records) {
        return Status::error(ErrorCode::ResourceLimitExceeded,
                             "journal holds more records than the configured bound");
    }

    JournalEntry stored = entry;
    stored.sequence = next_sequence_;
    ByteWriter payload_writer;
    encode(payload_writer, stored);
    if (!payload_writer.ok()) {
        return payload_writer.status();
    }

    std::vector<std::uint8_t> record;
    record.reserve(kRecordHeaderBytes + payload_writer.size());
    put_u16(record, static_cast<std::uint16_t>(stored.type));
    put_u16(record, 0);
    put_u32(record, static_cast<std::uint32_t>(payload_writer.size()));
    put_u64(record, stored.sequence);
    put_u32(record, crc32(payload_writer.buffer()));
    append_bytes(record, payload_writer.buffer().data(), payload_writer.size());

    std::vector<std::uint8_t> trailer;
    const std::uint32_t combined_crc = crc32_append(running_crc_, record);
    trailer.reserve(kTrailerBytes);
    append_bytes(trailer, reinterpret_cast<const std::uint8_t*>(kTrailerMagic), sizeof(kTrailerMagic));
    put_u64(trailer, static_cast<std::uint64_t>(entries_.size()) + 1u);
    put_u32(trailer, combined_crc);
    put_u32(trailer, 0);

    if (file_ != nullptr) {
#ifdef _WIN32
        if (_fseeki64(file_, static_cast<__int64>(valid_end_offset_), SEEK_SET) != 0) {
            failed_ = true;
            return Status::error(ErrorCode::PersistenceIoError, "journal seek failed");
        }
#else
        if (std::fseek(file_, static_cast<long>(valid_end_offset_), SEEK_SET) != 0) {
            failed_ = true;
            return Status::error(ErrorCode::PersistenceIoError, "journal seek failed");
        }
#endif
        Status status = write_bytes(record.data(), record.size());
        if (!status.ok()) {
            failed_ = true;
            return status;
        }
        status = write_bytes(trailer.data(), trailer.size());
        if (!status.ok()) {
            failed_ = true;
            return status;
        }
#ifdef _WIN32
        _chsize_s(_fileno(file_), static_cast<__int64>(valid_end_offset_ + record.size() + kTrailerBytes));
#endif
        status = flush();
        if (!status.ok()) {
            failed_ = true;
            return status;
        }
    }

    valid_end_offset_ += record.size();
    running_crc_ = combined_crc;
    next_sequence_ += 1;
    entries_.push_back(std::move(stored));
    return Status::success();
}

Status PersistentJournal::rewrite_all() {
    if (path_.empty()) {
        return Status::success();
    }
    if (failed_) {
        return Status::error(ErrorCode::PersistenceUnavailable, "journal has entered a failed state");
    }

    std::vector<std::uint8_t> content;
    append_bytes(content, reinterpret_cast<const std::uint8_t*>(kJournalMagic), sizeof(kJournalMagic));
    put_u32(content, kJournalFormatVersion);
    put_u32(content, 0);

    std::uint64_t sequence = 1;
    for (const JournalEntry& entry : entries_) {
        JournalEntry stored = entry;
        stored.sequence = sequence;
        ByteWriter payload_writer;
        encode(payload_writer, stored);
        if (!payload_writer.ok()) {
            return payload_writer.status();
        }
        put_u16(content, static_cast<std::uint16_t>(stored.type));
        put_u16(content, 0);
        put_u32(content, static_cast<std::uint32_t>(payload_writer.size()));
        put_u64(content, stored.sequence);
        put_u32(content, crc32(payload_writer.buffer()));
        append_bytes(content, payload_writer.buffer().data(), payload_writer.size());
        sequence += 1;
    }

    const std::uint32_t content_crc = crc32(content.data(), content.size());
    append_bytes(content, reinterpret_cast<const std::uint8_t*>(kTrailerMagic), sizeof(kTrailerMagic));
    put_u64(content, static_cast<std::uint64_t>(entries_.size()));
    put_u32(content, content_crc);
    put_u32(content, 0);

    const std::string temporary = path_ + ".tmp";
    Status status = write_file_sync(temporary, content);
    if (!status.ok()) {
        return status;
    }
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    status = atomic_replace(temporary, path_);
    if (!status.ok()) {
        return status;
    }
#ifdef _WIN32
    if (fopen_s(&file_, path_.c_str(), "r+b") != 0) {
        file_ = nullptr;
        failed_ = true;
        return Status::error(ErrorCode::PersistenceIoError, "unable to reopen the journal after replacement");
    }
#else
    file_ = std::fopen(path_.c_str(), "r+b");
    if (file_ == nullptr) {
        failed_ = true;
        return Status::error(ErrorCode::PersistenceIoError, "unable to reopen the journal after replacement");
    }
#endif
    valid_end_offset_ = content.size() - kTrailerBytes;
    running_crc_ = get_u32(content.data() + content.size() - 8);
    next_sequence_ = sequence;
    return Status::success();
}

Status PersistentJournal::close() {
    if (file_ != nullptr) {
        std::fflush(file_);
#ifdef _WIN32
        _commit(_fileno(file_));
#endif
        std::fclose(file_);
        file_ = nullptr;
    }
    return Status::success();
}

}  // namespace critic_fabric

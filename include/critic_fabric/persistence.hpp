// Critic Fabric — versioned, integrity-checked durable review state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_PERSISTENCE_HPP
#define CRITIC_FABRIC_PERSISTENCE_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "critic_fabric/authority.hpp"
#include "critic_fabric/challenge.hpp"
#include "critic_fabric/critic.hpp"
#include "critic_fabric/evidence.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/ids.hpp"
#include "critic_fabric/limits.hpp"
#include "critic_fabric/result.hpp"
#include "critic_fabric/review.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

// Journal record kinds. Numeric values are part of the durable format.
enum class JournalRecordType : std::uint16_t {
    Unspecified = 0,
    EpochEstablished = 1,
    TargetRegistered = 2,
    TargetSuperseded = 3,
    CriticRegistered = 4,
    WorkerRegistered = 5,
    ReviewDeclared = 6,
    AssignmentsAdded = 7,
    FindingRecorded = 8,
    FindingStateChanged = 9,
    EvidenceRecorded = 10,
    ChallengeRecorded = 11,
    RebuttalRecorded = 12,
    ReviewStateChanged = 13,
    DecisionCommitted = 14,
    AssignmentUpdated = 15,
};
inline constexpr std::uint16_t kJournalRecordTypeMax = 15;

const char* to_string(JournalRecordType type) noexcept;
bool is_valid_journal_record_type(std::uint16_t raw) noexcept;

// A mutable finding-state transition, recorded so replay reconstructs exactly
// which generation superseded which.
struct FindingStateChange {
    FindingId finding{};
    FindingState state = FindingState::Current;
    FindingId superseded_by{};
    ChallengeId challenged_by{};
    std::string reason;
};

struct ReviewStateChange {
    ReviewId review{};
    ReviewGeneration review_generation{};
    ReviewState state = ReviewState::Declared;
    ReviewDisposition disposition = ReviewDisposition::None;
    ReviewGeneration superseded_by{};
    std::string detail;
    std::string cancellation_reason;
    bool authority_current = true;
    std::uint32_t rounds_used = 0;
};

// One durable fact. Exactly one payload field is meaningful, selected by type.
struct JournalEntry {
    JournalRecordType type = JournalRecordType::Unspecified;
    std::uint64_t sequence = 0;

    CoordinatorEpoch epoch{};
    TargetRecord target;
    TargetRecord previous_target;
    TargetGeneration previous_target_generation{};
    CriticRegistration critic;
    WorkerRecord worker;
    ReviewRecord review;
    std::vector<CriticAssignment> assignments;
    CriticAssignment assignment;
    Finding finding;
    FindingStateChange finding_state;
    EvidenceRecord evidence;
    Challenge challenge;
    Rebuttal rebuttal;
    ReviewStateChange review_state;
    Decision decision;
};

void encode(class ByteWriter& writer, const FindingStateChange& value);
void encode(class ByteWriter& writer, const ReviewStateChange& value);
void encode(class ByteWriter& writer, const JournalEntry& value);
Status decode(class ByteReader& reader, FindingStateChange& out, const RuntimeLimits& limits);
Status decode(class ByteReader& reader, ReviewStateChange& out, const RuntimeLimits& limits);
Status decode(class ByteReader& reader, JournalEntry& out, const RuntimeLimits& limits);

// Durable append-only journal with an explicit end marker so that a truncated
// tail is detected rather than silently accepted.
//
// Layout: header(16) | record* | trailer(24)
// Record: type(u16) flags(u16) payload_length(u32) sequence(u64) crc32(u32) payload
//
// Every length is bounded, every checksum is verified, and the trailer carries
// the record count plus a running CRC over the header and all record bytes.
class PersistentJournal {
public:
    PersistentJournal() = default;
    ~PersistentJournal();

    PersistentJournal(const PersistentJournal&) = delete;
    PersistentJournal& operator=(const PersistentJournal&) = delete;

    static constexpr std::size_t kHeaderBytes = 16;
    static constexpr std::size_t kTrailerBytes = 24;
    static constexpr std::size_t kRecordHeaderBytes = 20;

    // Loads an existing journal, or establishes a new one when the file is
    // absent. Damage is never silently repaired.
    Status open(const std::string& path, const RuntimeLimits& limits);

    // Opens a non-durable in-process journal. Used when durability is not
    // configured; the coordinator still records the same entries so that the
    // recovery path is exercised identically.
    void open_in_memory(const RuntimeLimits& limits);

    Status append(const JournalEntry& entry);

    // Writes the complete entry list to a sibling temporary file and atomically
    // replaces the journal. Used for compaction and for an explicit durable
    // commit point.
    Status rewrite_all();

    Status close();

    [[nodiscard]] const std::vector<JournalEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] bool is_durable() const noexcept { return file_ != nullptr; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_sequence_; }

private:
    Status write_bytes(const std::uint8_t* data, std::size_t len);
    Status flush();

    std::FILE* file_ = nullptr;
    std::string path_;
    std::vector<JournalEntry> entries_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t valid_end_offset_ = 0;
    std::uint32_t running_crc_ = 0;
    RuntimeLimits limits_;
    bool failed_ = false;
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_PERSISTENCE_HPP

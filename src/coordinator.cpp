// Critic Fabric — the authoritative review coordinator.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/coordinator.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "critic_fabric/digest_domain.hpp"
#include "critic_fabric/lifecycle.hpp"
#include "critic_fabric/serialization.hpp"

namespace critic_fabric {
namespace {

bool capabilities_equal(const CriticCapability& a, const CriticCapability& b) {
    return a.roles == b.roles && a.target_classes == b.target_classes &&
           a.finding_categories == b.finding_categories && a.specialization == b.specialization &&
           a.implementation_version == b.implementation_version &&
           a.capability_evidence == b.capability_evidence &&
           a.deterministic_output == b.deterministic_output &&
           a.supports_challenge_response == b.supports_challenge_response;
}

// True when a finding still expresses an assertion about its target generation.
// Challenged and upheld findings remain current: a challenge is recorded, not a
// silent deletion.
bool finding_state_is_current(FindingState state) noexcept {
    return state == FindingState::Current || state == FindingState::Challenged ||
           state == FindingState::Upheld;
}

void assign_sequences(std::vector<JournalEntry>& entries, std::uint64_t next_sequence) {
    for (JournalEntry& entry : entries) {
        entry.sequence = next_sequence++;
    }
}

// The explicit, legal path from any open review state to EVALUATING. Emitting
// the intermediate states keeps the durable history a faithful record of how the
// review actually moved rather than a shortcut.
std::vector<ReviewState> path_to_evaluating(ReviewState from) {
    switch (from) {
        case ReviewState::Declared:
            return {ReviewState::Assigning, ReviewState::Assigned, ReviewState::Reviewing,
                    ReviewState::FindingsReceived, ReviewState::Validating, ReviewState::Evaluating};
        case ReviewState::Assigning:
            return {ReviewState::Assigned, ReviewState::Reviewing, ReviewState::FindingsReceived,
                    ReviewState::Validating, ReviewState::Evaluating};
        case ReviewState::Assigned:
            return {ReviewState::Reviewing, ReviewState::FindingsReceived, ReviewState::Validating,
                    ReviewState::Evaluating};
        case ReviewState::Reviewing:
            return {ReviewState::FindingsReceived, ReviewState::Validating, ReviewState::Evaluating};
        case ReviewState::FindingsReceived:
            return {ReviewState::Validating, ReviewState::Evaluating};
        case ReviewState::Validating:
        case ReviewState::Challenged:
        case ReviewState::RebuttalPending:
            return {ReviewState::Evaluating};
        case ReviewState::Evaluating:
            return {};
        default:
            return {};
    }
}

Digest compute_evidence_basis_digest(const std::map<FindingId, Finding>& findings,
                                     const std::map<EvidenceId, EvidenceRecord>& evidences) {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::State));
    writer.u32(static_cast<std::uint32_t>(findings.size()));
    for (const auto& entry : findings) {
        writer.u64(entry.first.value());
        writer.u32(static_cast<std::uint32_t>(entry.second.state));
        writer.digest(entry.second.finding_digest);
    }
    writer.u32(static_cast<std::uint32_t>(evidences.size()));
    for (const auto& entry : evidences) {
        writer.u64(entry.first.value());
        writer.boolean(entry.second.current);
        writer.boolean(entry.second.superseded);
        writer.u32(static_cast<std::uint32_t>(entry.second.integrity));
        writer.u32(static_cast<std::uint32_t>(entry.second.provenance));
        writer.digest(entry.second.content_digest);
    }
    return Sha256::hash(writer.buffer());
}

// True when an assignment has already been discharged by a current finding
// published under that exact incarnations. A critic that published its finding
// and then exited has completed its assignment; its later disappearance must not
// retroactively invalidate evidence-backed work.
bool assignment_is_discharged(const std::map<FindingId, Finding>& findings, CriticAssignmentId assignment) {
    for (const auto& entry : findings) {
        if (entry.second.assignment == assignment && finding_state_is_current(entry.second.state)) {
            return true;
        }
    }
    return false;
}

bool same_evidence_set(const std::vector<EvidenceId>& a, const std::vector<EvidenceId>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    std::vector<EvidenceId> left = a;
    std::vector<EvidenceId> right = b;
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

// The decision digest covers the durable, decision-relevant basis of an
// authoritative result: the review and target generations, the disposition, the
// policy that produced it, and the findings and evidence it rests on.
//
// It deliberately excludes the derived explanation digest, which contains
// process-incarnation identities such as the worker boot. Those are auditable
// through the explanation, but folding them into the decision digest would make
// two independent reviews of the same target generation under the same policy
// incomparable purely because different processes happened to run them.
Digest compute_decision_digest(const Decision& decision) {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::Decision));
    writer.u64(decision.review.value());
    writer.u64(decision.review_generation.value());
    writer.u64(decision.target.value());
    writer.u64(decision.target_generation.value());
    writer.u32(static_cast<std::uint32_t>(decision.disposition));
    writer.digest(decision.policy_digest);
    writer.digest(decision.evidence_basis_digest);
    return Sha256::hash(writer.buffer());
}

}  // namespace

struct Coordinator::ReviewSlot {
    ReviewRecord record;
    // Derived once when the review is declared or replayed. It is not persisted
    // because it is a pure function of the durable review specification.
    Digest policy_digest{};
    std::map<CriticAssignmentId, CriticAssignment> assignments;
    std::map<FindingId, Finding> findings;
    std::map<EvidenceId, EvidenceRecord> evidences;
    std::map<ChallengeId, Challenge> challenges;
    std::map<RebuttalId, Rebuttal> rebuttals;
    std::optional<Decision> decision;
};

Coordinator::Coordinator(CoordinatorConfig config) : config_(std::move(config)) {
    limits_ = config_.limits;
    limits_.clamp_to_hard_ceiling();
}

Coordinator::~Coordinator() {
    if (started_.load()) {
        stop();
    }
}

CoordinatorEpoch Coordinator::epoch() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return epoch_;
}

RecoveryReport Coordinator::recovery_report() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    return recovery_;
}

std::string Coordinator::persistence_path() const { return journal_.path(); }

Coordinator::ReviewSlot* Coordinator::find_slot_locked(ReviewId id) {
    const auto it = reviews_.find(id);
    return it == reviews_.end() ? nullptr : &it->second;
}

const Coordinator::ReviewSlot* Coordinator::find_slot_locked(ReviewId id) const {
    const auto it = reviews_.find(id);
    return it == reviews_.end() ? nullptr : &it->second;
}

const CriticAssignment* Coordinator::find_active_assignment_locked(const ReviewSlot& slot,
                                                                   CriticId critic) const {
    for (const auto& entry : slot.assignments) {
        if (entry.second.critic == critic && entry.second.active &&
            entry.second.review_generation == slot.record.generation) {
            return &entry.second;
        }
    }
    return nullptr;
}

// --- Journal plumbing --------------------------------------------------------

Status Coordinator::append_entries(const std::vector<JournalEntry>& entries) {
    for (const JournalEntry& entry : entries) {
        Status status = journal_.append(entry);
        if (!status.ok()) {
            // The in-memory change has already been applied and acknowledged.
            // The coordinator refuses every further authoritative mutation
            // rather than pretending the durable record exists.
            persistence_failed_.store(true);
            return status;
        }
    }
    return Status::success();
}

// --- Entry application (the single mutation path) ---------------------------

void Coordinator::apply_entry_locked(const JournalEntry& entry) {
    switch (entry.type) {
        case JournalRecordType::EpochEstablished:
            epoch_ = entry.epoch;
            break;
        case JournalRecordType::TargetRegistered:
            targets_[entry.target.id] = entry.target;
            break;
        case JournalRecordType::TargetSuperseded: {
            const TargetRecord previous = entry.previous_target;
            if (previous.id.valid() && previous.generation.valid()) {
                target_history_[previous.id][previous.generation] = previous;
            }
            targets_[entry.target.id] = entry.target;
            break;
        }
        case JournalRecordType::CriticRegistered:
            critics_[entry.critic.id] = entry.critic;
            break;
        case JournalRecordType::WorkerRegistered: {
            workers_[entry.worker.id] = entry.worker;
            const auto incumbent = incumbents_.find(entry.worker.critic);
            if (incumbent == incumbents_.end()) {
                incumbents_[entry.worker.critic] = entry.worker.id;
            } else {
                const auto existing = workers_.find(incumbent->second);
                const std::uint64_t existing_order =
                    existing == workers_.end() ? 0 : existing->second.boot_order;
                if (entry.worker.boot_order > existing_order) {
                    incumbents_[entry.worker.critic] = entry.worker.id;
                }
            }
            break;
        }
        case JournalRecordType::ReviewDeclared: {
            ReviewSlot& slot = reviews_[entry.review.id];
            slot.record = entry.review;
            slot.policy_digest = entry.review.spec.compute_policy_digest();
            review_requests_[entry.review.request] = entry.review.id;
            break;
        }
        case JournalRecordType::AssignmentsAdded: {
            ReviewSlot* slot = find_slot_locked(entry.review.id);
            if (slot != nullptr) {
                for (const CriticAssignment& assignment : entry.assignments) {
                    slot->assignments[assignment.id] = assignment;
                }
            }
            break;
        }
        case JournalRecordType::AssignmentUpdated: {
            ReviewSlot* slot = find_slot_locked(entry.assignment.review);
            if (slot != nullptr) {
                const auto it = slot->assignments.find(entry.assignment.id);
                if (it != slot->assignments.end()) {
                    it->second = entry.assignment;
                }
            }
            break;
        }
        case JournalRecordType::FindingRecorded: {
            ReviewSlot* slot = find_slot_locked(entry.finding.review);
            if (slot != nullptr) {
                slot->findings[entry.finding.id] = entry.finding;
                for (EvidenceId evidence_id : entry.finding.evidence) {
                    const auto it = slot->evidences.find(evidence_id);
                    if (it != slot->evidences.end() && !it->second.finding.valid()) {
                        it->second.finding = entry.finding.id;
                    }
                }
            }
            break;
        }
        case JournalRecordType::FindingStateChanged: {
            const FindingStateChange& change = entry.finding_state;
            ReviewSlot* slot = nullptr;
            for (auto& review_entry : reviews_) {
                if (review_entry.second.findings.count(change.finding) != 0) {
                    slot = &review_entry.second;
                    break;
                }
            }
            if (slot != nullptr) {
                Finding& finding = slot->findings[change.finding];
                finding.state = change.state;
                if (change.superseded_by.valid()) {
                    finding.supersedes = change.superseded_by;
                }
                if (change.challenged_by.valid()) {
                    finding.challenged_by = change.challenged_by;
                }
            }
            break;
        }
        case JournalRecordType::EvidenceRecorded: {
            ReviewSlot* slot = find_slot_locked(entry.evidence.review);
            if (slot != nullptr) {
                EvidenceRecord record = entry.evidence;
                if (record.finding.valid()) {
                    const auto it = slot->findings.find(record.finding);
                    if (it != slot->findings.end()) {
                        const bool already = std::find(it->second.evidence.begin(),
                                                       it->second.evidence.end(),
                                                       record.id) != it->second.evidence.end();
                        if (!already) {
                            it->second.evidence.push_back(record.id);
                        }
                    }
                }
                slot->evidences[record.id] = record;
            }
            break;
        }
        case JournalRecordType::ChallengeRecorded: {
            ReviewSlot* slot = find_slot_locked(entry.challenge.review);
            if (slot != nullptr) {
                slot->challenges[entry.challenge.id] = entry.challenge;
            }
            break;
        }
        case JournalRecordType::RebuttalRecorded: {
            ReviewSlot* slot = find_slot_locked(entry.rebuttal.review);
            if (slot != nullptr) {
                slot->rebuttals[entry.rebuttal.id] = entry.rebuttal;
            }
            break;
        }
        case JournalRecordType::ReviewStateChanged: {
            ReviewSlot* slot = find_slot_locked(entry.review_state.review);
            if (slot != nullptr && slot->record.generation == entry.review_state.review_generation) {
                slot->record.state = entry.review_state.state;
                slot->record.disposition = entry.review_state.disposition;
                slot->record.superseded_by = entry.review_state.superseded_by;
                if (!entry.review_state.detail.empty()) {
                    slot->record.status_detail = entry.review_state.detail;
                }
                slot->record.cancellation_reason = entry.review_state.cancellation_reason;
                slot->record.authority_current = entry.review_state.authority_current;
                slot->record.rounds_used = entry.review_state.rounds_used;
                slot->record.updated_order = entry.sequence;
            }
            break;
        }
        case JournalRecordType::DecisionCommitted: {
            ReviewSlot* slot = find_slot_locked(entry.decision.review);
            if (slot != nullptr && slot->record.generation == entry.decision.review_generation) {
                slot->decision = entry.decision;
            }
            break;
        }
        case JournalRecordType::Unspecified:
            break;
    }
}

void Coordinator::rebuild_derived_locked() {
    std::uint64_t max_order = 0;
    std::uint64_t target_max = 0;
    for (const auto& entry : targets_) {
        target_max = (std::max)(target_max, entry.first.value());
        max_order = (std::max)(max_order, entry.second.created_order);
    }
    for (const auto& entry : target_history_) {
        for (const auto& generation_entry : entry.second) {
            max_order = (std::max)(max_order, generation_entry.second.created_order);
        }
    }
    next_target_id_ = target_max + 1;

    std::uint64_t critic_max = 0;
    for (const auto& entry : critics_) {
        critic_max = (std::max)(critic_max, entry.first.value());
        max_order = (std::max)(max_order, entry.second.registered_order);
    }
    next_critic_id_ = critic_max + 1;

    std::uint64_t worker_max = 0;
    for (const auto& entry : workers_) {
        worker_max = (std::max)(worker_max, entry.first.value());
        max_order = (std::max)(max_order, entry.second.readiness_order);
        next_boot_order_ = (std::max)(next_boot_order_, entry.second.boot_order + 1);
    }
    next_worker_id_ = worker_max + 1;

    std::uint64_t review_max = 0;
    std::uint64_t assignment_max = 0;
    std::uint64_t finding_max = 0;
    std::uint64_t evidence_max = 0;
    std::uint64_t challenge_max = 0;
    std::uint64_t rebuttal_max = 0;
    std::uint64_t decision_max = 0;
    for (const auto& review_entry : reviews_) {
        review_max = (std::max)(review_max, review_entry.first.value());
        max_order = (std::max)(max_order, review_entry.second.record.updated_order);
        max_order = (std::max)(max_order, review_entry.second.record.declared_order);
        for (const auto& assignment : review_entry.second.assignments) {
            assignment_max = (std::max)(assignment_max, assignment.first.value());
            max_order = (std::max)(max_order, assignment.second.assigned_order);
        }
        for (const auto& finding : review_entry.second.findings) {
            finding_max = (std::max)(finding_max, finding.first.value());
            max_order = (std::max)(max_order, finding.second.creation_order);
        }
        for (const auto& evidence : review_entry.second.evidences) {
            evidence_max = (std::max)(evidence_max, evidence.first.value());
            max_order = (std::max)(max_order, evidence.second.created_order);
        }
        for (const auto& challenge : review_entry.second.challenges) {
            challenge_max = (std::max)(challenge_max, challenge.first.value());
            max_order = (std::max)(max_order, challenge.second.creation_order);
        }
        for (const auto& rebuttal : review_entry.second.rebuttals) {
            rebuttal_max = (std::max)(rebuttal_max, rebuttal.first.value());
            max_order = (std::max)(max_order, rebuttal.second.creation_order);
        }
        if (review_entry.second.decision.has_value()) {
            decision_max = (std::max)(decision_max, review_entry.second.decision->id.value());
            max_order = (std::max)(max_order, review_entry.second.decision->committed_order);
        }
    }
    next_review_id_ = review_max + 1;
    next_assignment_id_ = assignment_max + 1;
    next_finding_id_ = finding_max + 1;
    next_evidence_id_ = evidence_max + 1;
    next_challenge_id_ = challenge_max + 1;
    next_rebuttal_id_ = rebuttal_max + 1;
    next_decision_id_ = decision_max + 1;
    next_order_ = max_order + 1;
}

// --- Lifecycle ---------------------------------------------------------------

Status Coordinator::start() {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator is already started");
    }

    RecoveryReport report;
    if (config_.persistence_path.empty()) {
        journal_.open_in_memory(limits_);
    } else {
        Status status = journal_.open(config_.persistence_path, limits_);
        if (!status.ok()) {
            return status;
        }
        report.durable_state_recovered = true;
    }

    std::vector<JournalEntry> recovery_entries;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        targets_.clear();
        target_history_.clear();
        critics_.clear();
        workers_.clear();
        incumbents_.clear();
        reviews_.clear();
        review_requests_.clear();
        next_order_ = 1;
        next_target_id_ = 1;
        next_critic_id_ = 1;
        next_worker_id_ = 1;
        next_review_id_ = 1;
        next_assignment_id_ = 1;
        next_finding_id_ = 1;
        next_evidence_id_ = 1;
        next_challenge_id_ = 1;
        next_rebuttal_id_ = 1;
        next_decision_id_ = 1;
        next_boot_order_ = 1;

        CoordinatorEpoch previous{};
        for (const JournalEntry& entry : journal_.entries()) {
            if (entry.type == JournalRecordType::EpochEstablished) {
                previous = entry.epoch;
            }
            apply_entry_locked(entry);
        }
        rebuild_derived_locked();

        if (previous.advance_would_overflow()) {
            return Status::error(ErrorCode::ResourceLimitExceeded, "coordinator epoch space is exhausted");
        }
        const CoordinatorEpoch fresh = previous.valid() ? previous.advance() : CoordinatorEpoch::first();
        epoch_ = fresh;
        report.epoch = fresh;
        report.journal_records = static_cast<std::uint64_t>(journal_.entries().size());
        report.targets_recovered = static_cast<std::uint32_t>(targets_.size());
        report.critics_recovered = static_cast<std::uint32_t>(critics_.size());

        // Dynamic, process-local authority is discarded. A recovered worker is
        // known but not authoritative until it declares fresh readiness.
        for (auto& entry : workers_) {
            if (entry.second.ready || entry.second.healthy || entry.second.authoritative) {
                report.dynamic_authority_cleared += 1;
            }
            entry.second.ready = false;
            entry.second.healthy = false;
            entry.second.authoritative = false;
        }
        report.workers_recovered = static_cast<std::uint32_t>(workers_.size());

        for (auto& review_entry : reviews_) {
            ReviewSlot& slot = review_entry.second;
            report.reviews_recovered += 1;
            report.findings_recovered += static_cast<std::uint32_t>(slot.findings.size());
            if (slot.decision.has_value()) {
                report.decisions_recovered += 1;
            }
            if (!is_recoverable_review_state(slot.record.state)) {
                report.reviews_in_flight_at_shutdown += 1;
                ReviewStateChange change;
                change.review = slot.record.id;
                change.review_generation = slot.record.generation;
                change.state = ReviewState::RevalidationRequired;
                change.disposition = ReviewDisposition::RevalidationRequired;
                change.detail = "coordinator restarted: dynamic worker authority was cleared and must be "
                                "re-established before this review may produce a disposition";
                change.authority_current = false;
                change.rounds_used = slot.record.rounds_used;
                JournalEntry entry;
                entry.type = JournalRecordType::ReviewStateChanged;
                entry.review_state = change;
                recovery_entries.push_back(entry);
                report.reviews_moved_to_revalidation += 1;
            }
        }

        // The epoch is established first and recorded durably, so a later
        // restart advances rather than repeats it.
        JournalEntry epoch_entry;
        epoch_entry.type = JournalRecordType::EpochEstablished;
        epoch_entry.epoch = fresh;
        recovery_entries.insert(recovery_entries.begin(), epoch_entry);

        std::uint64_t sequence = journal_.next_sequence();
        for (JournalEntry& entry : recovery_entries) {
            entry.sequence = sequence++;
            apply_entry_locked(entry);
        }
        recovery_ = report;
    }

    Status status = append_entries(recovery_entries);
    if (!status.ok()) {
        return status;
    }
    started_.store(true);
    return Status::success();
}

Status Coordinator::stop() {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    journal_.close();
    started_.store(false);
    return Status::success();
}

// --- Identities --------------------------------------------------------------

TargetId Coordinator::allocate_target_id() {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    return TargetId::from_value(next_target_id_++);
}

CriticId Coordinator::allocate_critic_id() {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    return CriticId::from_value(next_critic_id_++);
}

WorkerId Coordinator::allocate_worker_id() {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    std::unique_lock<std::shared_mutex> lock(state_mutex_);
    return WorkerId::from_value(next_worker_id_++);
}

// --- Authority helpers -------------------------------------------------------

Status Coordinator::check_authority_locked(const AuthorityContext& authority, ReviewSlot** slot_out,
                                           const CriticAssignment** assignment_out, std::string& why) {
    if (slot_out != nullptr) {
        *slot_out = nullptr;
    }
    if (assignment_out != nullptr) {
        *assignment_out = nullptr;
    }
    if (authority.epoch != epoch_) {
        why = "message epoch does not match the current coordinator epoch";
        return Status::error(ErrorCode::StaleCoordinatorEpoch, why);
    }
    ReviewSlot* slot = find_slot_locked(authority.review);
    if (slot == nullptr) {
        why = "review is unknown to this coordinator";
        return Status::error(ErrorCode::ReviewUnknown, why);
    }
    if (slot->record.generation != authority.review_generation) {
        why = "message names a review generation that is no longer current";
        return Status::error(ErrorCode::StaleReviewGeneration, why);
    }
    if (slot->record.target != authority.target ||
        slot->record.target_generation != authority.target_generation) {
        why = "message target binding does not match the review target generation";
        return Status::error(ErrorCode::TargetMismatch, why);
    }
    switch (slot->record.state) {
        case ReviewState::Cancelled:
            why = "review was cancelled; no critic output can mutate it";
            return Status::error(ErrorCode::ReviewCancelled, why);
        case ReviewState::Superseded:
            why = "review was superseded; no critic output can mutate it";
            return Status::error(ErrorCode::ReviewSuperseded, why);
        case ReviewState::Committed:
            why = "review generation already produced an authoritative result";
            return Status::error(ErrorCode::ResultAlreadyCommitted, why);
        case ReviewState::Retired:
        case ReviewState::Failed:
            why = "review is closed";
            return Status::error(ErrorCode::ReviewClosed, why);
        default:
            break;
    }
    if (!accepts_critic_input(slot->record.state)) {
        why = std::string("review state ") + to_string(slot->record.state) + " does not accept critic input";
        return Status::error(ErrorCode::ReviewNotOpen, why);
    }

    const auto target_it = targets_.find(authority.target);
    if (target_it == targets_.end()) {
        why = "target is unknown";
        return Status::error(ErrorCode::TargetUnknown, why);
    }
    if (target_it->second.supersession != TargetSupersession::Current) {
        why = "target was superseded";
        return Status::error(ErrorCode::TargetSuperseded, why);
    }
    if (target_it->second.generation != slot->record.target_generation) {
        why = "target generation advanced; this review no longer describes the current target";
        return Status::error(ErrorCode::StaleTargetGeneration, why);
    }

    const auto critic_it = critics_.find(authority.critic);
    if (critic_it == critics_.end()) {
        why = "critic is not registered";
        return Status::error(ErrorCode::CriticNotRegistered, why);
    }
    if (critic_it->second.generation != authority.critic_generation) {
        why = "critic generation is no longer current";
        return Status::error(ErrorCode::StaleCriticGeneration, why);
    }
    const auto worker_it = workers_.find(authority.worker);
    if (worker_it == workers_.end()) {
        why = "worker incarnation is not registered";
        return Status::error(ErrorCode::WorkerNotRegistered, why);
    }
    if (worker_it->second.boot != authority.worker_boot) {
        why = "worker boot identity was superseded by a later incarnation";
        return Status::error(ErrorCode::StaleWorkerBoot, why);
    }
    if (worker_it->second.critic != authority.critic) {
        why = "worker incarnation is registered for a different critic identity";
        return Status::error(ErrorCode::CriticNotAuthoritative, why);
    }
    if (worker_it->second.critic_generation != authority.critic_generation) {
        why = "worker incarnation was registered against a different critic generation";
        return Status::error(ErrorCode::StaleCriticGeneration, why);
    }
    const auto incumbent_it = incumbents_.find(authority.critic);
    if (incumbent_it == incumbents_.end() || incumbent_it->second != authority.worker) {
        why = "another worker incarnation holds authority for this critic identity";
        return Status::error(ErrorCode::CriticNotAuthoritative, why);
    }
    if (!worker_it->second.ready || !worker_it->second.healthy || !worker_it->second.authoritative) {
        why = "worker incarnation has not declared current readiness evidence";
        return Status::error(ErrorCode::CriticNotAuthoritative, why);
    }

    const CriticAssignment* assignment = find_active_assignment_locked(*slot, authority.critic);
    if (assignment == nullptr) {
        why = "critic holds no active assignment for this review generation";
        return Status::error(ErrorCode::CriticNotAuthoritative, why);
    }
    if (assignment->worker != authority.worker || assignment->worker_boot != authority.worker_boot) {
        why = "assignment belongs to a different worker incarnation";
        return Status::error(ErrorCode::CriticNotAuthoritative, why);
    }
    if (assignment->critic_generation != authority.critic_generation) {
        why = "assignment was granted to a different critic generation";
        return Status::error(ErrorCode::StaleCriticGeneration, why);
    }
    if (slot_out != nullptr) {
        *slot_out = slot;
    }
    if (assignment_out != nullptr) {
        *assignment_out = assignment;
    }
    why.clear();
    return Status::success();
}

Status Coordinator::fence_worker_locked(WorkerId worker, std::string reason,
                                        std::vector<JournalEntry>& entries) {
    const auto it = workers_.find(worker);
    if (it == workers_.end()) {
        return Status::error(ErrorCode::WorkerNotRegistered, "cannot fence an unregistered worker incarnation");
    }
    const WorkerRecord previous = it->second;
    if (!previous.authoritative && !previous.ready && !previous.healthy) {
        return Status::success();
    }
    WorkerRecord fenced = previous;
    fenced.ready = false;
    fenced.healthy = false;
    fenced.authoritative = false;
    JournalEntry worker_entry;
    worker_entry.type = JournalRecordType::WorkerRegistered;
    worker_entry.worker = fenced;
    entries.push_back(worker_entry);

    for (auto& review_entry : reviews_) {
        ReviewSlot& slot = review_entry.second;
        bool touched = false;
        for (auto& assignment_entry : slot.assignments) {
            CriticAssignment assignment = assignment_entry.second;
            if (!assignment.active) {
                continue;
            }
            if (assignment.worker != previous.id || assignment.worker_boot != previous.boot) {
                continue;
            }
            if (assignment_is_discharged(slot.findings, assignment.id)) {
                // The incarnation already published a current finding, so the
                // assignment is complete. Only its right to publish more is lost.
                continue;
            }
            assignment.active = false;
            assignment.retirement_reason = reason;
            JournalEntry assignment_record;
            assignment_record.type = JournalRecordType::AssignmentUpdated;
            assignment_record.assignment = assignment;
            entries.push_back(assignment_record);
            touched = true;
        }
        if (touched) {
            Status status = revalidate_review_locked(slot, reason, entries);
            if (!status.ok()) {
                return status;
            }
        }
    }
    return Status::success();
}

Status Coordinator::revalidate_review_locked(ReviewSlot& slot, std::string reason,
                                             std::vector<JournalEntry>& entries) {
    if (is_recoverable_review_state(slot.record.state)) {
        return Status::success();
    }
    ReviewStateChange change;
    change.review = slot.record.id;
    change.review_generation = slot.record.generation;
    change.state = ReviewState::RevalidationRequired;
    change.disposition = ReviewDisposition::RevalidationRequired;
    change.detail = std::move(reason);
    change.authority_current = false;
    change.rounds_used = slot.record.rounds_used;
    JournalEntry entry;
    entry.type = JournalRecordType::ReviewStateChanged;
    entry.review_state = change;
    entries.push_back(entry);
    return Status::success();
}

std::vector<JournalEntry> Coordinator::supersede_reviews_for_target_locked(
    TargetId target, TargetGeneration previous_generation) {
    std::vector<JournalEntry> entries;
    for (auto& review_entry : reviews_) {
        ReviewSlot& slot = review_entry.second;
        if (slot.record.target != target || slot.record.target_generation != previous_generation) {
            continue;
        }
        for (auto& finding_entry : slot.findings) {
            if (!finding_state_is_current(finding_entry.second.state)) {
                continue;
            }
            FindingStateChange change;
            change.finding = finding_entry.first;
            change.state = FindingState::Invalidated;
            change.reason = "target generation superseded";
            JournalEntry entry;
            entry.type = JournalRecordType::FindingStateChanged;
            entry.finding_state = change;
            entries.push_back(entry);
        }

        ReviewStateChange change;
        change.review = slot.record.id;
        change.review_generation = slot.record.generation;
        change.rounds_used = slot.record.rounds_used;
        if (is_terminal_review_state(slot.record.state)) {
            if (!slot.record.authority_current) {
                continue;
            }
            change.state = slot.record.state;
            change.disposition = slot.record.disposition;
            change.detail = "target generation advanced; this historical outcome remains inspectable but no "
                            "longer describes the current target generation";
            change.authority_current = false;
        } else {
            change.state = ReviewState::Superseded;
            change.disposition = ReviewDisposition::Superseded;
            change.detail = "target generation advanced; a fresh review is required for the new generation";
            change.authority_current = false;
        }
        JournalEntry entry;
        entry.type = JournalRecordType::ReviewStateChanged;
        entry.review_state = change;
        entries.push_back(entry);
    }
    return entries;
}

LiveAuthorityView Coordinator::build_authority_view_locked(const ReviewSlot& slot) const {
    LiveAuthorityView view;
    view.epoch = epoch_;
    const auto target_it = targets_.find(slot.record.target);
    if (target_it != targets_.end()) {
        view.current_target = target_it->second.id;
        view.current_target_generation = target_it->second.generation;
        view.current_target_digest = target_it->second.content_digest;
    }
    // Ordered set, so the resulting maps are built in a deterministic order.
    std::set<CriticId> referenced;
    for (const auto& entry : slot.assignments) {
        referenced.insert(entry.second.critic);
    }
    for (const auto& entry : slot.findings) {
        referenced.insert(entry.second.critic);
    }
    // Incumbency and liveness are deliberately different questions. Liveness
    // governs the right to publish new critic output; incumbency governs whether
    // work already published is still bound to the current authority chain for
    // that critic identity. A critic that published its finding and then exited
    // keeps its finding current; a critic superseded by a later incarnation does
    // not.
    for (CriticId critic : referenced) {
        const auto critic_it = critics_.find(critic);
        if (critic_it != critics_.end()) {
            view.critic_generations[critic] = critic_it->second.generation;
        }
        const auto incumbent_it = incumbents_.find(critic);
        if (incumbent_it == incumbents_.end()) {
            continue;
        }
        const auto worker_it = workers_.find(incumbent_it->second);
        if (worker_it == workers_.end()) {
            continue;
        }
        WorkerIncarnation incarnation;
        incarnation.id = worker_it->second.id;
        incarnation.boot = worker_it->second.boot;
        incarnation.epoch = worker_it->second.epoch;
        view.incumbent_workers[critic] = incarnation;
    }
    return view;
}

EvaluationInput Coordinator::build_evaluation_input_locked(const ReviewSlot& slot) const {
    EvaluationInput input;
    input.review = slot.record;
    input.assignments.reserve(slot.assignments.size());
    for (const auto& entry : slot.assignments) {
        input.assignments.push_back(entry.second);
    }
    input.findings.reserve(slot.findings.size());
    for (const auto& entry : slot.findings) {
        input.findings.push_back(entry.second);
    }
    input.evidences.reserve(slot.evidences.size());
    for (const auto& entry : slot.evidences) {
        input.evidences.push_back(entry.second);
    }
    input.challenges.reserve(slot.challenges.size());
    for (const auto& entry : slot.challenges) {
        input.challenges.push_back(entry.second);
    }
    input.rebuttals.reserve(slot.rebuttals.size());
    for (const auto& entry : slot.rebuttals) {
        input.rebuttals.push_back(entry.second);
    }
    input.authority = build_authority_view_locked(slot);
    input.cancelled = slot.record.state == ReviewState::Cancelled;
    input.superseded = slot.record.state == ReviewState::Superseded;
    input.policy_digest = slot.policy_digest;
    return input;
}

ReviewSnapshot Coordinator::build_snapshot_locked(const ReviewSlot& slot) const {
    ReviewSnapshot snapshot;
    snapshot.review = slot.record;
    for (const auto& entry : slot.assignments) {
        snapshot.assignments.push_back(entry.second);
    }
    for (const auto& entry : slot.findings) {
        snapshot.findings.push_back(entry.second);
    }
    for (const auto& entry : slot.evidences) {
        snapshot.evidences.push_back(entry.second);
    }
    for (const auto& entry : slot.challenges) {
        snapshot.challenges.push_back(entry.second);
    }
    for (const auto& entry : slot.rebuttals) {
        snapshot.rebuttals.push_back(entry.second);
    }
    snapshot.decision = slot.decision;

    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::State));
    encode(writer, slot.record);
    writer.u32(static_cast<std::uint32_t>(slot.assignments.size()));
    for (const auto& entry : slot.assignments) {
        encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(slot.findings.size()));
    for (const auto& entry : slot.findings) {
        encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(slot.evidences.size()));
    for (const auto& entry : slot.evidences) {
        encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(slot.challenges.size()));
    for (const auto& entry : slot.challenges) {
        encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(slot.rebuttals.size()));
    for (const auto& entry : slot.rebuttals) {
        encode(writer, entry.second);
    }
    writer.boolean(slot.decision.has_value());
    if (slot.decision.has_value()) {
        encode(writer, *slot.decision);
    }
    snapshot.state_digest = Sha256::hash(writer.buffer());
    return snapshot;
}

// --- Target assertions -------------------------------------------------------

Result<TargetRecord> Coordinator::register_target(const RegisterTargetRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    TargetRecord result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "target registration carries a superseded coordinator epoch");
        }
        if (request.payload.size() > limits_.max_target_payload_bytes ||
            request.schema_name.size() > limits_.max_metadata_string_bytes ||
            request.provenance.size() > limits_.max_metadata_string_bytes ||
            request.producer_version.size() > limits_.max_metadata_string_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "target registration exceeds a configured metadata or payload bound");
        }

        TargetRecord record;
        record.id = request.id.valid() ? request.id : TargetId::from_value(next_target_id_++);
        record.target_class = request.target_class;
        record.schema_name = request.schema_name;
        record.payload = request.payload;
        record.schema_digest = Sha256::hash(request.schema_name);
        record.content_digest = record.compute_content_digest();
        record.producer = request.producer;
        record.producer_version = request.producer_version;
        record.provenance = request.provenance;
        record.reviewable = request.reviewable;
        record.supersession = TargetSupersession::Current;

        const auto existing_it = targets_.find(record.id);
        if (existing_it == targets_.end()) {
            if (targets_.size() >= limits_.max_retained_targets) {
                return Status::error(ErrorCode::ResourceLimitExceeded,
                                     "the coordinator holds more targets than the configured bound");
            }
            record.generation = TargetGeneration::first();
            record.created_order = next_order_++;
            JournalEntry entry;
            entry.type = JournalRecordType::TargetRegistered;
            entry.target = record;
            entries.push_back(entry);
        } else {
            const TargetRecord existing = existing_it->second;
            const bool identical = existing.content_digest == record.content_digest &&
                                   existing.target_class == record.target_class &&
                                   existing.schema_name == record.schema_name &&
                                   existing.reviewable == record.reviewable &&
                                   existing.supersession == TargetSupersession::Current;
            if (identical) {
                return existing;
            }
            if (existing.generation.advance_would_overflow()) {
                return Status::error(ErrorCode::ResourceLimitExceeded,
                                     "target generation space is exhausted");
            }
            record.generation = existing.generation.advance();
            record.created_order = next_order_++;
            JournalEntry entry;
            entry.type = JournalRecordType::TargetSuperseded;
            entry.previous_target = existing;
            entry.previous_target_generation = existing.generation;
            entry.target = record;
            entries.push_back(entry);
            std::vector<JournalEntry> derived =
                supersede_reviews_for_target_locked(record.id, existing.generation);
            entries.insert(entries.end(), derived.begin(), derived.end());
        }

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& entry : entries) {
            apply_entry_locked(entry);
        }
        result = record;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Result<TargetRecord> Coordinator::supersede_target(const SupersedeTargetRequest& request) {
    RegisterTargetRequest registration;
    registration.id = request.target;
    registration.target_class = request.target_class;
    registration.schema_name = request.schema_name;
    registration.payload = request.payload;
    registration.producer = request.producer;
    registration.producer_version = request.producer_version;
    registration.provenance = request.provenance;
    registration.reviewable = true;
    registration.epoch = request.epoch;
    registration.request = request.request;
    if (!request.target.valid()) {
        return Status::error(ErrorCode::InvalidIdentity, "target supersession requires an explicit target identity");
    }
    return register_target(registration);
}

Result<TargetRecord> Coordinator::query_target(TargetId id) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const auto it = targets_.find(id);
    if (it == targets_.end()) {
        return Status::error(ErrorCode::TargetUnknown, "target is unknown");
    }
    return it->second;
}

Result<TargetRecord> Coordinator::query_target_generation(TargetId id, TargetGeneration generation) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const auto current = targets_.find(id);
    if (current != targets_.end() && current->second.generation == generation) {
        return current->second;
    }
    const auto history = target_history_.find(id);
    if (history != target_history_.end()) {
        const auto generation_it = history->second.find(generation);
        if (generation_it != history->second.end()) {
            return generation_it->second;
        }
    }
    return Status::error(ErrorCode::TargetUnknown, "target generation is not retained");
}

// --- Critic and worker lifecycle --------------------------------------------

Result<CriticRegistration> Coordinator::register_critic(const RegisterCriticRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    CriticRegistration result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "critic registration carries a superseded coordinator epoch");
        }
        if (request.capability.roles.empty()) {
            return Status::error(ErrorCode::CriticCapabilityMismatch,
                                 "a critic must declare at least one role");
        }
        if (request.capability.target_classes.empty()) {
            return Status::error(ErrorCode::CriticCapabilityMismatch,
                                 "a critic must declare at least one supported target class");
        }
        if (request.capability.finding_categories.empty()) {
            return Status::error(ErrorCode::CriticCapabilityMismatch,
                                 "a critic must declare at least one supported finding category");
        }
        if (request.capability.specialization.size() > limits_.max_metadata_string_bytes ||
            request.capability.implementation_version.size() > limits_.max_metadata_string_bytes ||
            request.provenance.size() > limits_.max_metadata_string_bytes ||
            request.compat.schema_name.size() > limits_.max_metadata_string_bytes ||
            request.compat.producer_version.size() > limits_.max_metadata_string_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "critic registration exceeds a configured metadata bound");
        }

        CriticRegistration record;
        record.id = request.id.valid() ? request.id : CriticId::from_value(next_critic_id_++);
        record.capability = request.capability;
        record.registrant = request.registrant;
        record.provenance = request.provenance;
        record.compat = request.compat;

        const auto existing_it = critics_.find(record.id);
        if (existing_it == critics_.end()) {
            record.generation = CriticGeneration::first();
            record.registered_order = next_order_++;
            JournalEntry entry;
            entry.type = JournalRecordType::CriticRegistered;
            entry.critic = record;
            entries.push_back(entry);
        } else {
            const CriticRegistration existing = existing_it->second;
            // Only a material capability or compatibility change advances the
            // generation. A different provenance label from a second caller is
            // not an implementation change and must not retire live authority.
            if (!request.advance_generation && capabilities_equal(existing.capability, record.capability) &&
                existing.compat == record.compat) {
                return existing;
            }
            if (existing.generation.advance_would_overflow()) {
                return Status::error(ErrorCode::ResourceLimitExceeded,
                                     "critic generation space is exhausted");
            }
            record.generation = existing.generation.advance();
            record.registered_order = next_order_++;
            JournalEntry entry;
            entry.type = JournalRecordType::CriticRegistered;
            entry.critic = record;
            entries.push_back(entry);

            // Every incarnation and assignment of the previous generation is
            // fenced. A retired generation can never publish current findings.
            std::vector<WorkerId> incarnations;
            for (const auto& worker_entry : workers_) {
                if (worker_entry.second.critic == record.id) {
                    incarnations.push_back(worker_entry.first);
                }
            }
            for (WorkerId worker : incarnations) {
                Status status = fence_worker_locked(
                    worker, "critic implementation generation advanced; revalidation is required", entries);
                if (!status.ok()) {
                    return status;
                }
            }
            for (auto& review_entry : reviews_) {
                ReviewSlot& slot = review_entry.second;
                bool touched = false;
                for (auto& assignment_entry : slot.assignments) {
                    CriticAssignment assignment = assignment_entry.second;
                    if (!assignment.active || assignment.critic != record.id) {
                        continue;
                    }
                    // A critic implementation generation change retires the
                    // assignment unconditionally: findings produced by the
                    // retired implementation must not remain current, whether or
                    // not the incarnation already published one.
                    assignment.active = false;
                    assignment.retirement_reason =
                        "critic implementation generation advanced; revalidation is required";
                    JournalEntry assignment_record;
                    assignment_record.type = JournalRecordType::AssignmentUpdated;
                    assignment_record.assignment = assignment;
                    entries.push_back(assignment_record);
                    touched = true;
                }
                if (touched) {
                    Status status = revalidate_review_locked(
                        slot, "critic implementation generation advanced; revalidation is required", entries);
                    if (!status.ok()) {
                        return status;
                    }
                }
            }
        }

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& entry : entries) {
            apply_entry_locked(entry);
        }
        result = record;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Result<WorkerRecord> Coordinator::register_worker(const RegisterWorkerRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    WorkerRecord result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "worker registration carries a superseded coordinator epoch");
        }
        if (!request.boot.valid()) {
            return Status::error(ErrorCode::InvalidIdentity,
                                 "a worker incarnation must present a non-zero boot identity");
        }
        if (request.process_label.size() > limits_.max_metadata_string_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "worker registration exceeds a configured metadata bound");
        }
        const auto critic_it = critics_.find(request.critic);
        if (critic_it == critics_.end()) {
            return Status::error(ErrorCode::CriticNotRegistered,
                                 "worker registration names an unregistered critic identity");
        }
        if (critic_it->second.generation != request.critic_generation) {
            return Status::error(ErrorCode::StaleCriticGeneration,
                                 "worker registration names a critic generation that is not current");
        }

        const WorkerId worker_id = request.id.valid() ? request.id : WorkerId::from_value(next_worker_id_++);
        const auto existing_it = workers_.find(worker_id);
        if (existing_it != workers_.end() && existing_it->second.boot == request.boot) {
            if (existing_it->second.critic != request.critic ||
                existing_it->second.critic_generation != request.critic_generation) {
                return Status::error(ErrorCode::DuplicateWorkerIdentity,
                                     "this worker boot identity is already bound to a different critic");
            }
            return existing_it->second;
        }

        WorkerRecord record;
        record.id = worker_id;
        record.boot = request.boot;
        record.epoch = epoch_;
        record.critic = request.critic;
        record.critic_generation = request.critic_generation;
        record.host = request.host;
        record.process_label = request.process_label;
        record.ready = false;
        record.healthy = false;
        record.authoritative = false;
        record.readiness_order = next_order_++;

        if (existing_it != workers_.end()) {
            record.boot_order = next_boot_order_++;
            Status status = fence_worker_locked(
                record.id, "worker re-registered under a fresh boot identity; revalidation is required", entries);
            if (!status.ok()) {
                return status;
            }
        } else {
            if (workers_.size() >= limits_.max_worker_connections) {
                return Status::error(ErrorCode::ResourceLimitExceeded,
                                     "the coordinator holds more worker incarnations than the configured bound");
            }
            record.boot_order = next_boot_order_++;
        }
        if (next_boot_order_ == 0) {
            return Status::error(ErrorCode::ResourceLimitExceeded, "worker boot ordering space is exhausted");
        }

        // A second process claiming the same logical critic identity supersedes
        // the previous incarnation rather than adding a second vote.
        std::vector<WorkerId> competing;
        for (const auto& worker_entry : workers_) {
            if (worker_entry.first != record.id && worker_entry.second.critic == record.critic &&
                worker_entry.second.authoritative) {
                competing.push_back(worker_entry.first);
            }
        }
        for (WorkerId competitor : competing) {
            Status status = fence_worker_locked(
                competitor, "another worker incarnation claimed this critic identity", entries);
            if (!status.ok()) {
                return status;
            }
        }

        JournalEntry registration_entry;
        registration_entry.type = JournalRecordType::WorkerRegistered;
        registration_entry.worker = record;
        entries.push_back(registration_entry);

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = record;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Result<WorkerRecord> Coordinator::declare_readiness(const DeclareReadinessRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    WorkerRecord result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "readiness declaration carries a superseded coordinator epoch");
        }
        const auto worker_it = workers_.find(request.id);
        if (worker_it == workers_.end()) {
            return Status::error(ErrorCode::WorkerNotRegistered, "readiness names an unregistered worker");
        }
        if (worker_it->second.boot != request.boot) {
            return Status::error(ErrorCode::StaleWorkerBoot,
                                 "readiness carries a superseded worker boot identity");
        }
        if (worker_it->second.critic != request.critic) {
            return Status::error(ErrorCode::CriticNotAuthoritative,
                                 "readiness claims a critic identity this incarnation is not registered for");
        }
        const auto critic_it = critics_.find(request.critic);
        if (critic_it == critics_.end()) {
            return Status::error(ErrorCode::CriticNotRegistered, "readiness names an unregistered critic");
        }
        if (critic_it->second.generation != request.critic_generation ||
            worker_it->second.critic_generation != request.critic_generation) {
            return Status::error(ErrorCode::StaleCriticGeneration,
                                 "readiness names a critic generation that is not current");
        }
        const auto incumbent_it = incumbents_.find(request.critic);
        if (incumbent_it == incumbents_.end() || incumbent_it->second != request.id) {
            return Status::error(ErrorCode::CriticNotAuthoritative,
                                 "another worker incarnation holds authority for this critic identity");
        }
        if (request.ready) {
            if (request.capability_evidence != critic_it->second.capability.capability_evidence) {
                return Status::error(
                    ErrorCode::CriticCapabilityMismatch,
                    "readiness must carry the capability evidence registered for this critic generation; a "
                    "fresh incarnation cannot inherit authority from a previous process");
            }
        }

        WorkerRecord record = worker_it->second;
        record.epoch = epoch_;
        record.ready = request.ready;
        record.healthy = request.healthy;
        record.authoritative = request.ready && request.healthy;
        record.readiness_order = next_order_++;
        record.readiness_digest = request.capability_evidence;
        JournalEntry readiness_entry;
        readiness_entry.type = JournalRecordType::WorkerRegistered;
        readiness_entry.worker = record;
        entries.push_back(readiness_entry);

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = record;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Status Coordinator::fence_critic(CriticId critic, std::string reason) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    std::vector<JournalEntry> entries;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (critics_.find(critic) == critics_.end()) {
            return Status::error(ErrorCode::CriticNotRegistered, "cannot fence an unregistered critic");
        }
        std::vector<WorkerId> incarnations;
        for (const auto& worker_entry : workers_) {
            if (worker_entry.second.critic == critic) {
                incarnations.push_back(worker_entry.first);
            }
        }
        for (WorkerId worker : incarnations) {
            Status status = fence_worker_locked(worker, reason, entries);
            if (!status.ok()) {
                return status;
            }
        }
        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& entry : entries) {
            apply_entry_locked(entry);
        }
    }
    return append_entries(entries);
}

Status Coordinator::fence_worker(WorkerId worker, std::string reason) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    std::vector<JournalEntry> entries;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        Status status = fence_worker_locked(worker, std::move(reason), entries);
        if (!status.ok()) {
            return status;
        }
        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
    }
    return append_entries(entries);
}

Result<CriticRegistration> Coordinator::query_critic(CriticId id) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const auto it = critics_.find(id);
    if (it == critics_.end()) {
        return Status::error(ErrorCode::CriticNotRegistered, "critic is not registered");
    }
    return it->second;
}

Result<WorkerRecord> Coordinator::query_worker(WorkerId id) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const auto it = workers_.find(id);
    if (it == workers_.end()) {
        return Status::error(ErrorCode::WorkerNotRegistered, "worker is not registered");
    }
    return it->second;
}

std::vector<WorkerRecord> Coordinator::list_workers() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    std::vector<WorkerRecord> out;
    out.reserve(workers_.size());
    for (const auto& entry : workers_) {
        out.push_back(entry.second);
    }
    return out;
}

// --- Review declaration and assignment --------------------------------------

Result<ReviewRecord> Coordinator::declare_review(const DeclareReviewRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    ReviewRecord result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "review declaration carries a superseded coordinator epoch");
        }
        if (!request.request.valid()) {
            return Status::error(ErrorCode::InvalidIdentity,
                                 "a review declaration must carry a non-zero review request identity");
        }
        const auto duplicate = review_requests_.find(request.request);
        if (duplicate != review_requests_.end()) {
            const ReviewSlot* slot = find_slot_locked(duplicate->second);
            if (slot != nullptr && slot->record.target == request.target &&
                slot->record.target_generation == request.target_generation) {
                return slot->record;
            }
            return Status::error(ErrorCode::ReviewAlreadyExists,
                                 "this review request identity is already bound to a different target");
        }
        const auto target_it = targets_.find(request.target);
        if (target_it == targets_.end()) {
            return Status::error(ErrorCode::TargetUnknown, "review declaration names an unknown target");
        }
        if (target_it->second.generation != request.target_generation) {
            return Status::error(ErrorCode::StaleTargetGeneration,
                                 "review declaration names a target generation that is not current");
        }
        if (target_it->second.supersession != TargetSupersession::Current) {
            return Status::error(ErrorCode::TargetSuperseded,
                                 "review declaration names a superseded target");
        }
        if (!target_it->second.reviewable) {
            return Status::error(ErrorCode::TargetNotReviewable,
                                 "target is marked as not reviewable by its producer");
        }
        Status spec_status = validate_review_spec(request.spec);
        if (!spec_status.ok()) {
            return spec_status;
        }
        std::uint32_t in_flight = 0;
        for (const auto& review_entry : reviews_) {
            if (!is_terminal_review_state(review_entry.second.record.state)) {
                in_flight += 1;
            }
        }
        if (in_flight >= limits_.max_concurrent_reviews) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "the coordinator holds more in-flight reviews than the configured bound");
        }
        if (reviews_.size() >= limits_.max_retained_reviews) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "the coordinator holds more reviews than the configured bound");
        }

        ReviewRecord record;
        record.id = ReviewId::from_value(next_review_id_++);
        record.request = request.request;
        record.generation = ReviewGeneration::first();
        record.target = request.target;
        record.target_generation = request.target_generation;
        record.target_digest = target_it->second.content_digest;
        record.spec = request.spec;
        record.state = ReviewState::Declared;
        record.disposition = ReviewDisposition::None;
        record.declared_order = next_order_++;
        record.updated_order = record.declared_order;
        record.authority_current = true;

        JournalEntry entry;
        entry.type = JournalRecordType::ReviewDeclared;
        entry.review = record;
        entries.push_back(entry);

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = record;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Result<std::vector<CriticAssignment>> Coordinator::assign_critics(const AssignCriticsRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    std::vector<CriticAssignment> result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "assignment carries a superseded coordinator epoch");
        }
        ReviewSlot* slot = find_slot_locked(request.review);
        if (slot == nullptr) {
            return Status::error(ErrorCode::ReviewUnknown, "assignment names an unknown review");
        }
        if (slot->record.generation != request.review_generation) {
            return Status::error(ErrorCode::StaleReviewGeneration,
                                 "assignment names a review generation that is not current");
        }
        switch (slot->record.state) {
            case ReviewState::Cancelled:
                return Status::error(ErrorCode::ReviewCancelled, "review was cancelled");
            case ReviewState::Superseded:
                return Status::error(ErrorCode::ReviewSuperseded, "review was superseded");
            case ReviewState::Committed:
                return Status::error(ErrorCode::ResultAlreadyCommitted,
                                     "review generation already produced an authoritative result");
            case ReviewState::Failed:
            case ReviewState::Retired:
                return Status::error(ErrorCode::ReviewClosed, "review is closed");
            default:
                break;
        }
        const auto target_it = targets_.find(slot->record.target);
        if (target_it == targets_.end()) {
            return Status::error(ErrorCode::TargetUnknown, "review target is unknown");
        }
        if (target_it->second.generation != slot->record.target_generation) {
            return Status::error(ErrorCode::StaleTargetGeneration,
                                 "review target generation advanced; revalidation is required");
        }
        if (request.assignments.empty()) {
            return Status::error(ErrorCode::PolicyRejected, "no critic assignments were requested");
        }

        std::set<CriticId> requested;
        for (const AssignCriticsRequest::Assignment& requested_assignment : request.assignments) {
            if (!requested.insert(requested_assignment.critic).second) {
                return Status::error(ErrorCode::DuplicateCriticIdentity,
                                     "one assignment batch names the same logical critic twice");
            }
        }
        std::uint32_t active_assignments = 0;
        for (const auto& assignment_entry : slot->assignments) {
            if (assignment_entry.second.active) {
                active_assignments += 1;
            }
        }
        if (active_assignments + request.assignments.size() > limits_.max_critics_per_review) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "assignment would exceed the configured critics-per-review bound");
        }

        std::vector<CriticAssignment> created;
        for (const AssignCriticsRequest::Assignment& requested_assignment : request.assignments) {
            const auto critic_it = critics_.find(requested_assignment.critic);
            if (critic_it == critics_.end()) {
                return Status::error(ErrorCode::CriticNotRegistered, "assignment names an unregistered critic");
            }
            if (critic_it->second.generation != requested_assignment.critic_generation) {
                return Status::error(ErrorCode::StaleCriticGeneration,
                                     "assignment names a critic generation that is not current");
            }
            if (find_active_assignment_locked(*slot, requested_assignment.critic) != nullptr) {
                return Status::error(
                    ErrorCode::DuplicateCriticIdentity,
                    "critic already holds an active assignment for this review generation; one logical critic "
                    "may never contribute twice to the same review");
            }
            const CriticCapability& capability = critic_it->second.capability;
            if (!capability.declares_role(requested_assignment.role)) {
                return Status::error(ErrorCode::CriticCapabilityMismatch,
                                     "critic does not declare the role it is being assigned");
            }
            if (!capability.declares_target_class(target_it->second.target_class)) {
                return Status::error(ErrorCode::CriticCapabilityMismatch,
                                     "critic does not declare support for this target class");
            }
            for (const auto& category_entry : slot->record.spec.required_min_categories) {
                if (!capability.declares_category(category_entry.first)) {
                    return Status::error(ErrorCode::CriticCapabilityMismatch,
                                         "critic does not declare a finding category the policy requires");
                }
            }

            const auto worker_it = workers_.find(requested_assignment.worker);
            if (worker_it == workers_.end()) {
                return Status::error(ErrorCode::WorkerNotRegistered,
                                     "assignment names an unregistered worker incarnation");
            }
            if (worker_it->second.boot != requested_assignment.worker_boot) {
                return Status::error(ErrorCode::StaleWorkerBoot,
                                     "assignment names a superseded worker boot identity");
            }
            if (worker_it->second.critic != requested_assignment.critic ||
                worker_it->second.critic_generation != requested_assignment.critic_generation) {
                return Status::error(ErrorCode::CriticNotAuthoritative,
                                     "worker incarnation is not registered for this critic generation");
            }
            const auto incumbent_it = incumbents_.find(requested_assignment.critic);
            if (incumbent_it == incumbents_.end() || incumbent_it->second != requested_assignment.worker) {
                return Status::error(ErrorCode::CriticNotAuthoritative,
                                     "another worker incarnation holds authority for this critic identity");
            }
            if (!worker_it->second.ready || !worker_it->second.healthy ||
                !worker_it->second.authoritative) {
                return Status::error(ErrorCode::RequiredCriticUnavailable,
                                     "worker incarnation has not declared current readiness evidence");
            }

            CriticAssignment assignment;
            assignment.id = CriticAssignmentId::from_value(next_assignment_id_++);
            assignment.review = slot->record.id;
            assignment.review_generation = slot->record.generation;
            assignment.target = slot->record.target;
            assignment.target_generation = slot->record.target_generation;
            assignment.critic = requested_assignment.critic;
            assignment.critic_generation = requested_assignment.critic_generation;
            assignment.worker = requested_assignment.worker;
            assignment.worker_boot = requested_assignment.worker_boot;
            assignment.role = requested_assignment.role;
            assignment.required = requested_assignment.required ||
                                  slot->record.spec.is_required_role(requested_assignment.role);
            assignment.weight = slot->record.spec.weight_of(requested_assignment.critic);
            assignment.assigned_order = next_order_++;
            assignment.active = true;
            assignment.attempt = AttemptGeneration::first();
            created.push_back(assignment);
        }

        const ReviewState original = slot->record.state;
        auto push_state = [&](ReviewState next, ReviewDisposition disposition, std::string detail) {
            ReviewStateChange change;
            change.review = slot->record.id;
            change.review_generation = slot->record.generation;
            change.state = next;
            change.disposition = disposition;
            change.detail = std::move(detail);
            change.authority_current = true;
            change.rounds_used = slot->record.rounds_used;
            JournalEntry entry;
            entry.type = JournalRecordType::ReviewStateChanged;
            entry.review_state = change;
            entries.push_back(entry);
        };
        if (original == ReviewState::Declared) {
            push_state(ReviewState::Assigning, ReviewDisposition::None, "assigning critics");
        }
        JournalEntry batch;
        batch.type = JournalRecordType::AssignmentsAdded;
        batch.review.id = slot->record.id;
        batch.review.generation = slot->record.generation;
        batch.assignments = created;
        entries.push_back(batch);
        if (original == ReviewState::Declared || original == ReviewState::Assigning ||
            original == ReviewState::RevalidationRequired) {
            if (original == ReviewState::RevalidationRequired) {
                push_state(ReviewState::Assigning, ReviewDisposition::None,
                           "revalidation: reassigning critics under current authority");
            }
            push_state(ReviewState::Assigned, ReviewDisposition::None,
                       "critics assigned; review may accept findings");
        }

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& entry : entries) {
            apply_entry_locked(entry);
        }
        result = created;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

// --- Critic output -----------------------------------------------------------

Result<EvidenceRecord> Coordinator::submit_evidence(const SubmitEvidenceRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    EvidenceRecord result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        AuthorityContext authority;
        authority.epoch = request.epoch;
        authority.worker = request.worker;
        authority.worker_boot = request.worker_boot;
        authority.critic = request.critic;
        authority.critic_generation = request.critic_generation;
        authority.review = request.review;
        authority.review_generation = request.review_generation;
        authority.target = request.target;
        authority.target_generation = request.target_generation;
        authority.request = request.request;
        ReviewSlot* slot = nullptr;
        const CriticAssignment* assignment = nullptr;
        std::string why;
        Status status = check_authority_locked(authority, &slot, &assignment, why);
        if (!status.ok()) {
            return status;
        }
        (void)assignment;
        if (request.payload.size() > limits_.max_evidence_payload_bytes ||
            request.reference.size() > limits_.max_metadata_string_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "evidence exceeds a configured payload or metadata bound");
        }
        if (slot->evidences.size() >= limits_.max_evidence_records_per_review) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "review holds more evidence records than the configured bound");
        }

        EvidenceRecord record;
        record.id = EvidenceId::from_value(next_evidence_id_++);
        record.generation = EvidenceGeneration::first();
        record.source_type = request.source_type;
        record.provenance = request.provenance;
        record.integrity = request.integrity;
        record.producer = request.producer;
        record.target = slot->record.target;
        record.target_generation = slot->record.target_generation;
        record.target_digest = slot->record.target_digest;
        record.review = slot->record.id;
        record.review_generation = slot->record.generation;
        record.critic = request.critic;
        record.critic_generation = request.critic_generation;
        record.worker = request.worker;
        record.worker_boot = request.worker_boot;
        record.reference = request.reference;
        record.payload = request.payload;
        record.content_digest = record.compute_content_digest();
        record.created_order = next_order_++;
        record.current = true;
        record.superseded = false;

        if (request.finding.valid()) {
            const auto finding_it = slot->findings.find(request.finding);
            if (finding_it == slot->findings.end()) {
                return Status::error(ErrorCode::FindingUnknown,
                                     "evidence names a finding this review does not hold");
            }
            if (finding_it->second.critic != request.critic) {
                return Status::error(ErrorCode::MalformedEvidence,
                                     "evidence may only be bound to a finding produced by the same critic");
            }
            if (!finding_state_is_current(finding_it->second.state)) {
                return Status::error(ErrorCode::FindingSuperseded,
                                     "evidence may not be bound to a finding that is no longer current");
            }
            record.finding = request.finding;
        }

        JournalEntry entry;
        entry.type = JournalRecordType::EvidenceRecorded;
        entry.evidence = record;
        entries.push_back(entry);

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = record;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Result<Finding> Coordinator::submit_finding(const SubmitFindingRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    Finding result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        AuthorityContext authority;
        authority.epoch = request.epoch;
        authority.worker = request.worker;
        authority.worker_boot = request.worker_boot;
        authority.critic = request.critic;
        authority.critic_generation = request.critic_generation;
        authority.review = request.review;
        authority.review_generation = request.review_generation;
        authority.target = request.target;
        authority.target_generation = request.target_generation;
        authority.request = request.request;
        ReviewSlot* slot = nullptr;
        const CriticAssignment* assignment = nullptr;
        std::string why;
        Status status = check_authority_locked(authority, &slot, &assignment, why);
        if (!status.ok()) {
            return status;
        }
        if (request.explanation.size() > limits_.max_explanation_bytes ||
            request.provenance.size() > limits_.max_metadata_string_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "finding exceeds a configured explanation or metadata bound");
        }
        if (request.evidence.size() > limits_.max_evidence_per_finding) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "finding references more evidence records than the configured bound");
        }
        if (slot->findings.size() >= limits_.max_findings_per_review) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "review holds more findings than the configured bound");
        }
        for (EvidenceId evidence_id : request.evidence) {
            const auto evidence_it = slot->evidences.find(evidence_id);
            if (evidence_it == slot->evidences.end()) {
                return Status::error(ErrorCode::MalformedFinding,
                                     "finding references evidence this review does not hold");
            }
            const EvidenceRecord& evidence = evidence_it->second;
            if (!evidence.current || evidence.superseded) {
                return Status::error(ErrorCode::EvidenceNotCurrent,
                                     "finding references evidence that is no longer current");
            }
            if (evidence.review != slot->record.id ||
                evidence.review_generation != slot->record.generation) {
                return Status::error(ErrorCode::EvidenceNotCurrent,
                                     "finding references evidence from a different review generation");
            }
            if (evidence.target != slot->record.target ||
                evidence.target_generation != slot->record.target_generation) {
                return Status::error(ErrorCode::TargetMismatch,
                                     "finding references evidence bound to a different target generation");
            }
            if (evidence.critic != request.critic ||
                evidence.critic_generation != request.critic_generation) {
                return Status::error(ErrorCode::EvidenceNotCurrent,
                                     "finding references evidence produced by a different critic generation");
            }
            if (evidence.finding.valid()) {
                const auto bound_it = slot->findings.find(evidence.finding);
                if (bound_it == slot->findings.end() ||
                    bound_it->second.critic != request.critic) {
                    return Status::error(
                        ErrorCode::MalformedFinding,
                        "finding references evidence already bound to another critic's finding");
                }
            }
        }

        Finding candidate;
        candidate.critic = request.critic;
        candidate.critic_generation = request.critic_generation;
        candidate.assignment = assignment->id;
        candidate.worker = request.worker;
        candidate.worker_boot = request.worker_boot;
        candidate.attempt = assignment->attempt;
        candidate.review = slot->record.id;
        candidate.review_generation = slot->record.generation;
        candidate.target = slot->record.target;
        candidate.target_generation = slot->record.target_generation;
        candidate.category = request.category;
        candidate.severity = request.severity;
        candidate.outcome = request.outcome;
        candidate.evidence = request.evidence;
        candidate.explanation = request.explanation;
        candidate.confidence = request.confidence;
        candidate.provenance = request.provenance;
        candidate.finding_digest = candidate.compute_finding_digest();

        for (const auto& finding_entry : slot->findings) {
            const Finding& existing = finding_entry.second;
            if (existing.critic != request.critic ||
                existing.review_generation != slot->record.generation) {
                continue;
            }
            if (!finding_state_is_current(existing.state)) {
                continue;
            }
            if (existing.finding_digest == candidate.finding_digest) {
                switch (slot->record.spec.duplicates) {
                    case DuplicatePolicy::Reject:
                        return Status::error(
                            ErrorCode::DuplicateFinding,
                            "an identical finding from this critic is already recorded for this review generation");
                    case DuplicatePolicy::IdempotentIgnore:
                        return existing;
                    case DuplicatePolicy::KeepDistinct:
                        break;
                }
            } else if (existing.category == candidate.category &&
                       same_evidence_set(existing.evidence, candidate.evidence) &&
                       is_decisive_outcome(existing.outcome) &&
                       is_decisive_outcome(candidate.outcome) &&
                       existing.outcome != candidate.outcome) {
                return Status::error(
                    ErrorCode::ConflictingFinding,
                    "this critic already recorded a contradictory decisive outcome for the same category and "
                    "evidence under this review generation; the earlier finding must be superseded explicitly");
            }
        }

        if (request.supersedes.valid()) {
            const auto superseded_it = slot->findings.find(request.supersedes);
            if (superseded_it == slot->findings.end()) {
                return Status::error(ErrorCode::FindingUnknown, "finding to supersede is unknown");
            }
            if (superseded_it->second.critic != request.critic) {
                return Status::error(ErrorCode::MalformedFinding,
                                     "a critic may only supersede a finding it produced itself");
            }
            if (!finding_state_is_current(superseded_it->second.state)) {
                return Status::error(ErrorCode::FindingSuperseded,
                                     "finding to supersede is no longer current");
            }
            if (superseded_it->second.generation.advance_would_overflow()) {
                return Status::error(ErrorCode::ResourceLimitExceeded,
                                     "finding generation space is exhausted");
            }
            candidate.generation = superseded_it->second.generation.advance();
            candidate.supersedes = request.supersedes;
            FindingStateChange change;
            change.finding = request.supersedes;
            change.state = FindingState::Superseded;
            change.superseded_by = FindingId{};
            change.reason = "superseded by a later finding generation from the same critic";
            JournalEntry state_entry;
            state_entry.type = JournalRecordType::FindingStateChanged;
            state_entry.finding_state = change;
            entries.push_back(state_entry);
        } else {
            candidate.generation = FindingGeneration::first();
        }

        candidate.id = FindingId::from_value(next_finding_id_++);
        candidate.creation_order = next_order_++;
        candidate.state = FindingState::Current;
        candidate.finding_digest = candidate.compute_finding_digest();

        JournalEntry entry;
        entry.type = JournalRecordType::FindingRecorded;
        entry.finding = candidate;
        entries.push_back(entry);

        if (slot->record.state == ReviewState::Assigned || slot->record.state == ReviewState::Reviewing) {
            ReviewStateChange change;
            change.review = slot->record.id;
            change.review_generation = slot->record.generation;
            change.state = ReviewState::FindingsReceived;
            change.disposition = ReviewDisposition::None;
            change.detail = "critic findings received";
            change.authority_current = true;
            change.rounds_used = slot->record.rounds_used;
            JournalEntry state_entry;
            state_entry.type = JournalRecordType::ReviewStateChanged;
            state_entry.review_state = change;
            entries.push_back(state_entry);
        }

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = candidate;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

// --- Challenge and rebuttal --------------------------------------------------

Result<Challenge> Coordinator::submit_challenge(const SubmitChallengeRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    Challenge result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        AuthorityContext authority;
        authority.epoch = request.epoch;
        authority.worker = request.worker;
        authority.worker_boot = request.worker_boot;
        authority.critic = request.critic;
        authority.critic_generation = request.critic_generation;
        authority.review = request.review;
        authority.review_generation = request.review_generation;
        authority.target = request.target;
        authority.target_generation = request.target_generation;
        authority.request = request.request;
        ReviewSlot* slot = nullptr;
        const CriticAssignment* assignment = nullptr;
        std::string why;
        Status status = check_authority_locked(authority, &slot, &assignment, why);
        if (!status.ok()) {
            return status;
        }
        (void)assignment;
        if (slot->record.spec.challenges == ChallengePolicy::Disabled) {
            return Status::error(ErrorCode::ChallengeDisabled,
                                 "review policy does not permit challenges");
        }
        if (request.disputed_predicate.size() > limits_.max_metadata_string_bytes ||
            request.rationale.size() > limits_.max_explanation_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "challenge exceeds a configured explanation or metadata bound");
        }
        if (request.evidence.size() > limits_.max_evidence_per_finding) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "challenge references more evidence records than the configured bound");
        }
        if (slot->challenges.size() >= limits_.max_challenges_per_review) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "review holds more challenges than the configured bound");
        }
        const auto finding_it = slot->findings.find(request.challenged_finding);
        if (finding_it == slot->findings.end()) {
            return Status::error(ErrorCode::FindingUnknown, "challenge names an unknown finding");
        }
        const Finding& challenged = finding_it->second;
        if (challenged.generation != request.challenged_finding_generation) {
            return Status::error(ErrorCode::StaleChallenge,
                                 "challenge names a finding generation that is not current");
        }
        if (!finding_state_is_current(challenged.state)) {
            return Status::error(ErrorCode::StaleChallenge,
                                 "challenge names a finding that is no longer current");
        }
        if (challenged.review_generation != slot->record.generation ||
            challenged.target_generation != slot->record.target_generation) {
            return Status::error(ErrorCode::StaleChallenge,
                                 "challenge names a finding bound to a different review or target generation");
        }
        if (challenged.critic == request.critic) {
            return Status::error(ErrorCode::InvalidChallenge,
                                 "a critic may not challenge its own finding");
        }
        std::uint32_t rounds_used = 0;
        for (const auto& challenge_entry : slot->challenges) {
            if (challenge_entry.second.challenged_finding == request.challenged_finding) {
                rounds_used += 1;
            }
        }
        if (rounds_used >= slot->record.spec.max_challenge_rounds) {
            return Status::error(ErrorCode::ChallengeLimitExceeded,
                                 "the bounded challenge rounds for this finding are exhausted");
        }
        for (EvidenceId evidence_id : request.evidence) {
            const auto evidence_it = slot->evidences.find(evidence_id);
            if (evidence_it == slot->evidences.end()) {
                return Status::error(ErrorCode::InvalidChallenge,
                                     "challenge references evidence this review does not hold");
            }
            if (!evidence_it->second.current || evidence_it->second.superseded) {
                return Status::error(ErrorCode::InvalidChallenge,
                                     "challenge references evidence that is no longer current");
            }
        }

        Challenge challenge;
        challenge.id = ChallengeId::from_value(next_challenge_id_++);
        challenge.generation = ChallengeGeneration::from_value(static_cast<std::uint64_t>(rounds_used) + 1u);
        challenge.challenged_finding = challenged.id;
        challenge.challenged_finding_generation = challenged.generation;
        challenge.challenger = request.critic;
        challenge.challenger_generation = request.critic_generation;
        challenge.worker = request.worker;
        challenge.worker_boot = request.worker_boot;
        challenge.review = slot->record.id;
        challenge.review_generation = slot->record.generation;
        challenge.target = slot->record.target;
        challenge.target_generation = slot->record.target_generation;
        challenge.disputed_predicate = request.disputed_predicate;
        challenge.rationale = request.rationale;
        challenge.evidence = request.evidence;
        challenge.creation_order = next_order_++;
        challenge.outcome = ChallengeOutcome::Unresolved;
        challenge.resolved = false;

        JournalEntry entry;
        entry.type = JournalRecordType::ChallengeRecorded;
        entry.challenge = challenge;
        entries.push_back(entry);

        FindingStateChange finding_change;
        finding_change.finding = challenged.id;
        finding_change.state = FindingState::Challenged;
        finding_change.challenged_by = challenge.id;
        finding_change.reason = "challenged by another authoritative critic";
        JournalEntry finding_entry;
        finding_entry.type = JournalRecordType::FindingStateChanged;
        finding_entry.finding_state = finding_change;
        entries.push_back(finding_entry);

        if (slot->record.state != ReviewState::Challenged) {
            ReviewStateChange review_change;
            review_change.review = slot->record.id;
            review_change.review_generation = slot->record.generation;
            review_change.state = ReviewState::Challenged;
            review_change.disposition = ReviewDisposition::None;
            review_change.detail = "a finding was challenged; the review is in a bounded challenge round";
            review_change.authority_current = true;
            review_change.rounds_used = slot->record.rounds_used;
            JournalEntry review_entry;
            review_entry.type = JournalRecordType::ReviewStateChanged;
            review_entry.review_state = review_change;
            entries.push_back(review_entry);
        }

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = challenge;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Result<Rebuttal> Coordinator::submit_rebuttal(const SubmitRebuttalRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    Rebuttal result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        AuthorityContext authority;
        authority.epoch = request.epoch;
        authority.worker = request.worker;
        authority.worker_boot = request.worker_boot;
        authority.critic = request.critic;
        authority.critic_generation = request.critic_generation;
        authority.review = request.review;
        authority.review_generation = request.review_generation;
        authority.target = request.target;
        authority.target_generation = request.target_generation;
        authority.request = request.request;
        ReviewSlot* slot = nullptr;
        const CriticAssignment* assignment = nullptr;
        std::string why;
        Status status = check_authority_locked(authority, &slot, &assignment, why);
        if (!status.ok()) {
            return status;
        }
        (void)assignment;
        if (slot->record.spec.challenges == ChallengePolicy::Disabled) {
            return Status::error(ErrorCode::ChallengeDisabled, "review policy does not permit rebuttals");
        }
        if (request.argument.size() > limits_.max_explanation_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "rebuttal exceeds the configured explanation bound");
        }
        if (request.evidence.size() > limits_.max_evidence_per_finding) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "rebuttal references more evidence records than the configured bound");
        }
        const auto challenge_it = slot->challenges.find(request.challenge);
        if (challenge_it == slot->challenges.end()) {
            return Status::error(ErrorCode::ChallengeUnknown, "rebuttal names an unknown challenge");
        }
        const Challenge challenge = challenge_it->second;
        if (challenge.generation != request.challenge_generation) {
            return Status::error(ErrorCode::StaleRebuttal,
                                 "rebuttal names a challenge generation that is not current");
        }
        if (challenge.review_generation != slot->record.generation ||
            challenge.target_generation != slot->record.target_generation) {
            return Status::error(ErrorCode::StaleRebuttal,
                                 "rebuttal names a challenge bound to a different review generation");
        }
        if (challenge.resolved) {
            return Status::error(ErrorCode::StaleRebuttal, "challenge has already been answered");
        }
        const auto finding_it = slot->findings.find(challenge.challenged_finding);
        if (finding_it == slot->findings.end()) {
            return Status::error(ErrorCode::FindingUnknown,
                                 "rebuttal names a challenge whose finding is unknown");
        }
        if (finding_it->second.critic != request.critic) {
            return Status::error(ErrorCode::InvalidRebuttal,
                                 "only the critic that produced the challenged finding may rebut it");
        }
        if (!finding_state_is_current(finding_it->second.state)) {
            return Status::error(ErrorCode::StaleChallenge,
                                 "the challenged finding is no longer current");
        }
        std::uint32_t existing_rebuttals = 0;
        for (const auto& rebuttal_entry : slot->rebuttals) {
            if (rebuttal_entry.second.challenge == challenge.id) {
                existing_rebuttals += 1;
            }
        }
        if (existing_rebuttals >= limits_.max_rebuttals_per_challenge) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "this challenge already holds the maximum number of rebuttals");
        }
        for (EvidenceId evidence_id : request.evidence) {
            const auto evidence_it = slot->evidences.find(evidence_id);
            if (evidence_it == slot->evidences.end()) {
                return Status::error(ErrorCode::InvalidRebuttal,
                                     "rebuttal references evidence this review does not hold");
            }
            if (!evidence_it->second.current || evidence_it->second.superseded) {
                return Status::error(ErrorCode::InvalidRebuttal,
                                     "rebuttal references evidence that is no longer current");
            }
        }

        Rebuttal rebuttal;
        rebuttal.id = RebuttalId::from_value(next_rebuttal_id_++);
        rebuttal.challenge = challenge.id;
        rebuttal.challenge_generation = challenge.generation;
        rebuttal.finding = challenge.challenged_finding;
        rebuttal.author = request.critic;
        rebuttal.author_generation = request.critic_generation;
        rebuttal.worker = request.worker;
        rebuttal.worker_boot = request.worker_boot;
        rebuttal.review = slot->record.id;
        rebuttal.review_generation = slot->record.generation;
        rebuttal.target = slot->record.target;
        rebuttal.target_generation = slot->record.target_generation;
        rebuttal.argument = request.argument;
        rebuttal.evidence = request.evidence;
        rebuttal.creation_order = next_order_++;

        JournalEntry rebuttal_entry;
        rebuttal_entry.type = JournalRecordType::RebuttalRecorded;
        rebuttal_entry.rebuttal = rebuttal;
        entries.push_back(rebuttal_entry);

        Challenge resolved = challenge;
        resolved.outcome = request.outcome;
        resolved.resolved = true;
        resolved.rebuttal = rebuttal.id;
        JournalEntry challenge_entry;
        challenge_entry.type = JournalRecordType::ChallengeRecorded;
        challenge_entry.challenge = resolved;
        entries.push_back(challenge_entry);

        FindingState next_state = FindingState::Challenged;
        switch (request.outcome) {
            case ChallengeOutcome::Upheld:
                next_state = FindingState::Upheld;
                break;
            case ChallengeOutcome::Modified:
            case ChallengeOutcome::Superseded:
                next_state = FindingState::Superseded;
                break;
            case ChallengeOutcome::Invalidated:
                next_state = FindingState::Invalidated;
                break;
            case ChallengeOutcome::Unresolved:
                next_state = FindingState::Challenged;
                break;
        }
        FindingStateChange finding_change;
        finding_change.finding = challenge.challenged_finding;
        finding_change.state = next_state;
        finding_change.challenged_by = challenge.id;
        finding_change.reason = std::string("challenge rebuttal resolved as ") + to_string(request.outcome);
        JournalEntry finding_entry;
        finding_entry.type = JournalRecordType::FindingStateChanged;
        finding_entry.finding_state = finding_change;
        entries.push_back(finding_entry);

        ReviewStateChange review_change;
        review_change.review = slot->record.id;
        review_change.review_generation = slot->record.generation;
        review_change.state = ReviewState::Evaluating;
        review_change.disposition = ReviewDisposition::None;
        review_change.detail = std::string("challenge rebuttal resolved as ") + to_string(request.outcome);
        review_change.authority_current = true;
        review_change.rounds_used = slot->record.rounds_used + 1;
        JournalEntry review_entry;
        review_entry.type = JournalRecordType::ReviewStateChanged;
        review_entry.review_state = review_change;
        entries.push_back(review_entry);

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = rebuttal;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

// --- Authoritative result ----------------------------------------------------

Result<Decision> Coordinator::commit_review(const CommitReviewRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    Decision result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "commit carries a superseded coordinator epoch");
        }
        ReviewSlot* slot = find_slot_locked(request.review);
        if (slot == nullptr) {
            return Status::error(ErrorCode::ReviewUnknown, "commit names an unknown review");
        }
        if (slot->record.generation != request.review_generation) {
            return Status::error(ErrorCode::StaleReviewGeneration,
                                 "commit names a review generation that is not current");
        }
        if (slot->record.target_generation != request.target_generation) {
            return Status::error(ErrorCode::StaleTargetGeneration,
                                 "commit names a target generation that is not the review's target generation");
        }
        switch (slot->record.state) {
            case ReviewState::Cancelled:
                return Status::error(ErrorCode::ReviewCancelled,
                                     "a cancelled review cannot commit a result");
            case ReviewState::Superseded:
                return Status::error(ErrorCode::ReviewSuperseded,
                                     "a superseded review cannot commit a result");
            case ReviewState::Failed:
            case ReviewState::Retired:
                return Status::error(ErrorCode::ReviewClosed, "review is closed");
            case ReviewState::RevalidationRequired:
                return Status::error(
                    ErrorCode::RevalidationRequired,
                    "recovered or fenced review authority must be re-established and the review reassigned "
                    "before an authoritative result may be committed");
            default:
                break;
        }

        EvaluationInput input = build_evaluation_input_locked(*slot);
        EvaluationResult evaluation = evaluate_review(input);
        const Digest policy_digest = slot->policy_digest.is_zero()
                                        ? slot->record.spec.compute_policy_digest()
                                        : slot->policy_digest;
        const Digest evidence_basis = compute_evidence_basis_digest(slot->findings, slot->evidences);

        if (slot->decision.has_value()) {
            Decision recomputed = *slot->decision;
            recomputed.disposition = evaluation.disposition;
            recomputed.policy_digest = policy_digest;
            recomputed.evidence_basis_digest = evidence_basis;
            const Digest digest = compute_decision_digest(recomputed);
            if (digest == slot->decision->decision_digest) {
                return *slot->decision;
            }
            return Status::error(
                ErrorCode::ConflictingReviewCommit,
                "a different authoritative result is already committed for this review generation; at most one "
                "logical result may be committed per generation");
        }
        if (slot->record.state == ReviewState::Committed) {
            return Status::error(ErrorCode::ResultAlreadyCommitted,
                                 "review is marked committed but holds no decision record");
        }

        for (ReviewState next : path_to_evaluating(slot->record.state)) {
            ReviewStateChange change;
            change.review = slot->record.id;
            change.review_generation = slot->record.generation;
            change.state = next;
            change.disposition = next == ReviewState::Evaluating ? evaluation.disposition
                                                                 : ReviewDisposition::None;
            change.detail = "aggregating authoritative critic output";
            change.authority_current = true;
            change.rounds_used = slot->record.rounds_used;
            JournalEntry entry;
            entry.type = JournalRecordType::ReviewStateChanged;
            entry.review_state = change;
            entries.push_back(entry);
        }

        Decision decision;
        decision.id = DecisionId::from_value(next_decision_id_++);
        decision.generation = DecisionGeneration::first();
        decision.review = slot->record.id;
        decision.review_generation = slot->record.generation;
        decision.target = slot->record.target;
        decision.target_generation = slot->record.target_generation;
        decision.disposition = evaluation.disposition;
        decision.policy_digest = policy_digest;
        decision.evidence_basis_digest = evidence_basis;
        decision.committed_order = next_order_++;
        decision.epoch = epoch_;
        decision.decision_digest = compute_decision_digest(decision);

        ReviewStateChange resolved;
        resolved.review = slot->record.id;
        resolved.review_generation = slot->record.generation;
        resolved.state = ReviewState::Resolved;
        resolved.disposition = evaluation.disposition;
        resolved.detail = evaluation.explanation.policy_result;
        resolved.authority_current = true;
        resolved.rounds_used = slot->record.rounds_used;
        JournalEntry resolved_entry;
        resolved_entry.type = JournalRecordType::ReviewStateChanged;
        resolved_entry.review_state = resolved;
        entries.push_back(resolved_entry);

        JournalEntry decision_entry;
        decision_entry.type = JournalRecordType::DecisionCommitted;
        decision_entry.decision = decision;
        entries.push_back(decision_entry);

        ReviewStateChange committed;
        committed.review = slot->record.id;
        committed.review_generation = slot->record.generation;
        committed.state = ReviewState::Committed;
        committed.disposition = evaluation.disposition;
        committed.detail = "authoritative review result committed";
        committed.authority_current = true;
        committed.rounds_used = slot->record.rounds_used;
        JournalEntry committed_entry;
        committed_entry.type = JournalRecordType::ReviewStateChanged;
        committed_entry.review_state = committed;
        entries.push_back(committed_entry);

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = decision;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

Result<ReviewRecord> Coordinator::cancel_review(const CancelReviewRequest& request) {
    if (!started_.load()) {
        return Status::error(ErrorCode::InternalError, "coordinator has not been started");
    }
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    if (persistence_failed_.load()) {
        return Status::error(ErrorCode::PersistenceUnavailable,
                             "durable state is unavailable; no further authoritative mutation is accepted");
    }
    std::vector<JournalEntry> entries;
    ReviewRecord result;
    {
        std::unique_lock<std::shared_mutex> lock(state_mutex_);
        if (request.epoch != epoch_) {
            return Status::error(ErrorCode::StaleCoordinatorEpoch,
                                 "cancellation carries a superseded coordinator epoch");
        }
        if (request.reason.size() > limits_.max_metadata_string_bytes) {
            return Status::error(ErrorCode::ResourceLimitExceeded,
                                 "cancellation reason exceeds the configured metadata bound");
        }
        ReviewSlot* slot = find_slot_locked(request.review);
        if (slot == nullptr) {
            return Status::error(ErrorCode::ReviewUnknown, "cancellation names an unknown review");
        }
        if (slot->record.generation != request.review_generation) {
            return Status::error(ErrorCode::StaleReviewGeneration,
                                 "cancellation names a review generation that is not current");
        }
        if (slot->record.state == ReviewState::Cancelled) {
            return slot->record;
        }
        if (slot->record.state == ReviewState::Committed) {
            return Status::error(ErrorCode::ReviewClosed,
                                 "a committed authoritative result cannot be cancelled; open a new review "
                                 "generation instead");
        }
        if (slot->record.state == ReviewState::Superseded) {
            return Status::error(ErrorCode::ReviewSuperseded, "review was already superseded");
        }
        if (slot->record.state == ReviewState::Retired || slot->record.state == ReviewState::Failed) {
            return Status::error(ErrorCode::ReviewClosed, "review is already closed");
        }

        ReviewStateChange change;
        change.review = slot->record.id;
        change.review_generation = slot->record.generation;
        change.state = ReviewState::Cancelled;
        change.disposition = ReviewDisposition::Cancelled;
        change.detail = "review cancelled";
        change.cancellation_reason = request.reason;
        change.authority_current = false;
        change.rounds_used = slot->record.rounds_used;
        JournalEntry entry;
        entry.type = JournalRecordType::ReviewStateChanged;
        entry.review_state = change;
        entries.push_back(entry);

        assign_sequences(entries, journal_.next_sequence());
        for (const JournalEntry& journal_entry : entries) {
            apply_entry_locked(journal_entry);
        }
        result = slot->record;
    }
    Status status = append_entries(entries);
    if (!status.ok()) {
        return status;
    }
    return result;
}

// --- Queries -----------------------------------------------------------------

Result<ReviewSnapshot> Coordinator::query_review(ReviewId id) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const ReviewSlot* slot = find_slot_locked(id);
    if (slot == nullptr) {
        return Status::error(ErrorCode::ReviewUnknown, "review is unknown");
    }
    return build_snapshot_locked(*slot);
}

Result<Decision> Coordinator::query_decision(ReviewId id, ReviewGeneration generation) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const ReviewSlot* slot = find_slot_locked(id);
    if (slot == nullptr) {
        return Status::error(ErrorCode::ReviewUnknown, "review is unknown");
    }
    if (slot->record.generation != generation) {
        return Status::error(ErrorCode::StaleReviewGeneration,
                             "decision query names a review generation that is not current");
    }
    if (!slot->decision.has_value()) {
        return Status::error(ErrorCode::DecisionUnknown,
                             "no authoritative result has been committed for this review generation");
    }
    return *slot->decision;
}

Result<Explanation> Coordinator::explain_review(ReviewId id) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const ReviewSlot* slot = find_slot_locked(id);
    if (slot == nullptr) {
        return Status::error(ErrorCode::ReviewUnknown, "review is unknown");
    }
    EvaluationInput input = build_evaluation_input_locked(*slot);
    EvaluationResult evaluation = evaluate_review(input);
    return evaluation.explanation;
}

Result<EvaluationResult> Coordinator::evaluate_review_now(ReviewId id) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    const ReviewSlot* slot = find_slot_locked(id);
    if (slot == nullptr) {
        return Status::error(ErrorCode::ReviewUnknown, "review is unknown");
    }
    EvaluationInput input = build_evaluation_input_locked(*slot);
    return evaluate_review(input);
}

std::vector<ReviewSummary> Coordinator::list_reviews() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    std::vector<ReviewSummary> out;
    out.reserve(reviews_.size());
    for (const auto& entry : reviews_) {
        const ReviewSlot& slot = entry.second;
        ReviewSummary summary;
        summary.id = slot.record.id;
        summary.generation = slot.record.generation;
        summary.target = slot.record.target;
        summary.target_generation = slot.record.target_generation;
        summary.state = slot.record.state;
        summary.disposition = slot.record.disposition;
        summary.finding_count = static_cast<std::uint32_t>(slot.findings.size());
        for (const auto& finding_entry : slot.findings) {
            if (finding_state_is_current(finding_entry.second.state)) {
                summary.current_finding_count += 1;
            }
        }
        for (const auto& assignment_entry : slot.assignments) {
            if (assignment_entry.second.active) {
                summary.assignment_count += 1;
            }
        }
        summary.has_decision = slot.decision.has_value();
        out.push_back(summary);
    }
    return out;
}

std::vector<Finding> Coordinator::list_findings(ReviewId id) const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    std::vector<Finding> out;
    const ReviewSlot* slot = find_slot_locked(id);
    if (slot == nullptr) {
        return out;
    }
    out.reserve(slot->findings.size());
    for (const auto& entry : slot->findings) {
        out.push_back(entry.second);
    }
    return out;
}

Digest Coordinator::authoritative_state_digest() const {
    std::shared_lock<std::shared_mutex> lock(state_mutex_);
    ByteWriter writer;
    // Process-local authority such as the coordinator epoch is deliberately
    // excluded: two coordinators holding identical durable history must agree.
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::State));
    writer.u32(static_cast<std::uint32_t>(targets_.size()));
    for (const auto& entry : targets_) {
        encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(critics_.size()));
    for (const auto& entry : critics_) {
        encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(reviews_.size()));
    for (const auto& entry : reviews_) {
        encode(writer, entry.second.record);
        writer.u32(static_cast<std::uint32_t>(entry.second.assignments.size()));
        for (const auto& assignment : entry.second.assignments) {
            encode(writer, assignment.second);
        }
        writer.u32(static_cast<std::uint32_t>(entry.second.findings.size()));
        for (const auto& finding : entry.second.findings) {
            encode(writer, finding.second);
        }
        writer.u32(static_cast<std::uint32_t>(entry.second.evidences.size()));
        for (const auto& evidence : entry.second.evidences) {
            encode(writer, evidence.second);
        }
        writer.u32(static_cast<std::uint32_t>(entry.second.challenges.size()));
        for (const auto& challenge : entry.second.challenges) {
            encode(writer, challenge.second);
        }
        writer.u32(static_cast<std::uint32_t>(entry.second.rebuttals.size()));
        for (const auto& rebuttal : entry.second.rebuttals) {
            encode(writer, rebuttal.second);
        }
        writer.boolean(entry.second.decision.has_value());
        if (entry.second.decision.has_value()) {
            encode(writer, *entry.second.decision);
        }
    }
    return Sha256::hash(writer.buffer());
}

}  // namespace critic_fabric

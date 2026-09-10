// Critic Fabric — the authoritative review coordinator.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_COORDINATOR_HPP
#define CRITIC_FABRIC_COORDINATOR_HPP

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "critic_fabric/authority.hpp"
#include "critic_fabric/challenge.hpp"
#include "critic_fabric/critic.hpp"
#include "critic_fabric/digest.hpp"
#include "critic_fabric/evaluation.hpp"
#include "critic_fabric/evidence.hpp"
#include "critic_fabric/explanation.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/limits.hpp"
#include "critic_fabric/persistence.hpp"
#include "critic_fabric/result.hpp"
#include "critic_fabric/review.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

struct CoordinatorConfig {
    RuntimeLimits limits;
    // Empty path selects a non-durable in-process journal.
    std::string persistence_path;
    ProducerId coordinator_id{};
    std::string process_label = "coordinator";
};

// What happened when this coordinator established its epoch. Reported so an
// operator can see exactly which dynamic authority was discarded.
struct RecoveryReport {
    CoordinatorEpoch epoch{};
    bool durable_state_recovered = false;
    std::uint64_t journal_records = 0;
    std::uint32_t targets_recovered = 0;
    std::uint32_t critics_recovered = 0;
    std::uint32_t workers_recovered = 0;
    std::uint32_t reviews_recovered = 0;
    std::uint32_t reviews_in_flight_at_shutdown = 0;
    std::uint32_t reviews_moved_to_revalidation = 0;
    std::uint32_t decisions_recovered = 0;
    std::uint32_t findings_recovered = 0;
    std::uint32_t dynamic_authority_cleared = 0;
};

// The coordinator owns every authoritative mutation. All query APIs return
// detached value copies; no caller ever holds a reference into mutable internal
// state.
//
// Thread-safety: every public method is safe to call concurrently. Mutating
// methods are serialized by an internal commit lock; read-only methods take a
// shared lock and never block one another.
//
// Lock ownership: commit_mutex_ is always the outermost lock and is the only
// lock held while the durable journal is written; state_mutex_ is taken
// exclusively only for mutations and shared for queries, and is never held
// across file, socket, or backend work. There is no third lock.
class Coordinator {
public:
    explicit Coordinator(CoordinatorConfig config);
    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    // Establishes a fresh coordinator epoch and recovers durable state. A review
    // that was in flight when a previous coordinator stopped is moved to
    // REVALIDATION_REQUIRED; it is never resumed on the strength of persisted
    // dynamic evidence.
    Status start();
    Status stop();

    [[nodiscard]] CoordinatorEpoch epoch() const;
    [[nodiscard]] const RuntimeLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] RecoveryReport recovery_report() const;
    [[nodiscard]] bool persistence_failed() const noexcept { return persistence_failed_.load(); }
    [[nodiscard]] std::string persistence_path() const;

    // --- Durable assertions --------------------------------------------------
    Result<TargetRecord> register_target(const RegisterTargetRequest& request);
    Result<TargetRecord> supersede_target(const SupersedeTargetRequest& request);
    Result<CriticRegistration> register_critic(const RegisterCriticRequest& request);
    Result<WorkerRecord> register_worker(const RegisterWorkerRequest& request);
    Result<WorkerRecord> declare_readiness(const DeclareReadinessRequest& request);

    Result<ReviewRecord> declare_review(const DeclareReviewRequest& request);
    Result<std::vector<CriticAssignment>> assign_critics(const AssignCriticsRequest& request);

    Result<EvidenceRecord> submit_evidence(const SubmitEvidenceRequest& request);
    Result<Finding> submit_finding(const SubmitFindingRequest& request);
    Result<Challenge> submit_challenge(const SubmitChallengeRequest& request);
    Result<Rebuttal> submit_rebuttal(const SubmitRebuttalRequest& request);

    Result<Decision> commit_review(const CommitReviewRequest& request);
    Result<ReviewRecord> cancel_review(const CancelReviewRequest& request);

    // --- Queries -------------------------------------------------------------
    Result<TargetRecord> query_target(TargetId id) const;
    Result<TargetRecord> query_target_generation(TargetId id, TargetGeneration generation) const;
    Result<CriticRegistration> query_critic(CriticId id) const;
    Result<WorkerRecord> query_worker(WorkerId id) const;
    Result<ReviewSnapshot> query_review(ReviewId id) const;
    Result<Decision> query_decision(ReviewId id, ReviewGeneration generation) const;
    Result<Explanation> explain_review(ReviewId id) const;
    Result<EvaluationResult> evaluate_review_now(ReviewId id) const;
    std::vector<ReviewSummary> list_reviews() const;
    std::vector<WorkerRecord> list_workers() const;
    std::vector<Finding> list_findings(ReviewId id) const;

    // Canonical digest over the durable state that determines review outcomes.
    // Process-local authority such as the coordinator epoch is excluded, so two
    // coordinators holding identical committed history produce identical
    // digests.
    [[nodiscard]] Digest authoritative_state_digest() const;

    // Fences every worker incarnation for one logical critic, for example after
    // an operator observes a hung process. Late traffic from a fenced
    // incarnation is rejected before any state mutation.
    Status fence_critic(CriticId critic, std::string reason);
    // Fences exactly one worker incarnation. Used when a connection is lost or
    // an operator observes a hung process: the boot identity is retired and its
    // late traffic is rejected before any state mutation.
    Status fence_worker(WorkerId worker, std::string reason);

    // Allocates the next identity of each kind.
    TargetId allocate_target_id();
    CriticId allocate_critic_id();
    WorkerId allocate_worker_id();

private:
    struct ReviewSlot;

    // Every authoritative mutation flows through this: validate under the state
    // lock, compute the journal entries, apply them through apply_entry_locked,
    // then persist with only the commit lock held. One code path means replay
    // can never drift from live behavior.
    Status commit_entries(std::vector<JournalEntry> entries);

    void apply_entry_locked(const JournalEntry& entry);
    void rebuild_derived_locked();
    ReviewSlot* find_slot_locked(ReviewId id);
    const ReviewSlot* find_slot_locked(ReviewId id) const;
    const CriticAssignment* find_active_assignment_locked(const ReviewSlot& slot, CriticId critic) const;
    Status fence_worker_locked(WorkerId worker, std::string reason, std::vector<JournalEntry>& entries);
    Status revalidate_review_locked(ReviewSlot& slot, std::string reason, std::vector<JournalEntry>& entries);
    std::vector<JournalEntry> supersede_reviews_for_target_locked(TargetId target,
                                                                  TargetGeneration previous_generation);
    // Builds the authority view over exactly the critics this review references,
    // so evaluation cost scales with the review rather than with the total
    // number of registered critics.
    LiveAuthorityView build_authority_view_locked(const ReviewSlot& slot) const;
    EvaluationInput build_evaluation_input_locked(const ReviewSlot& slot) const;
    ReviewSnapshot build_snapshot_locked(const ReviewSlot& slot) const;
    Status check_authority_locked(const AuthorityContext& authority, ReviewSlot** slot_out,
                                  const CriticAssignment** assignment_out, std::string& why);
    Status append_entries(const std::vector<JournalEntry>& entries);

    CoordinatorConfig config_;
    RuntimeLimits limits_;
    mutable std::mutex commit_mutex_;
    mutable std::shared_mutex state_mutex_;
    PersistentJournal journal_;
    std::atomic<bool> persistence_failed_{false};
    std::atomic<bool> started_{false};
    RecoveryReport recovery_;

    // --- State owned exclusively by state_mutex_ -----------------------------
    CoordinatorEpoch epoch_{};
    std::map<TargetId, TargetRecord> targets_;
    std::map<TargetId, std::map<TargetGeneration, TargetRecord>> target_history_;
    std::map<CriticId, CriticRegistration> critics_;
    std::map<WorkerId, WorkerRecord> workers_;
    std::map<CriticId, WorkerId> incumbents_;
    std::map<ReviewId, ReviewSlot> reviews_;
    std::map<ReviewRequestId, ReviewId> review_requests_;
    std::uint64_t next_order_ = 1;
    std::uint64_t next_target_id_ = 1;
    std::uint64_t next_critic_id_ = 1;
    std::uint64_t next_worker_id_ = 1;
    std::uint64_t next_review_id_ = 1;
    std::uint64_t next_assignment_id_ = 1;
    std::uint64_t next_finding_id_ = 1;
    std::uint64_t next_evidence_id_ = 1;
    std::uint64_t next_challenge_id_ = 1;
    std::uint64_t next_rebuttal_id_ = 1;
    std::uint64_t next_decision_id_ = 1;
    std::uint64_t next_boot_order_ = 1;
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_COORDINATOR_HPP

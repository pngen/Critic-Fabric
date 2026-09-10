// Critic Fabric — deterministic race tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every race is ordered with an explicit latch. The expected legal winner is
// decided before the race starts and the loser must observe a typed rejection.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/worker.hpp"
#include "test_support.hpp"

using namespace critic_fabric;
using namespace cftest;

namespace {

// A reusable latch: wait() blocks until exactly count participants have arrived.
class Latch {
public:
    explicit Latch(int count) : threshold_(count), count_(count) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const int generation = generation_;
        count_ -= 1;
        if (count_ == 0) {
            generation_ += 1;
            count_ = threshold_;
            condition_.notify_all();
            return;
        }
        condition_.wait(lock, [this, generation] { return generation_ != generation; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    int threshold_;
    int count_;
    int generation_ = 0;
};

struct Setup {
    CoordinatorEpoch epoch{};
    TargetRecord target;
    ReviewRecord review;
    struct CriticRecord {
        CriticId id{};
        CriticGeneration generation{};
        WorkerId worker{};
        WorkerBootId boot{};
    };
    std::vector<CriticRecord> critics;
    std::vector<CriticAssignment> assignments;
    SyntheticCriticConfig template_config;
};

CoordinatorConfig make_config() {
    CoordinatorConfig config;
    config.process_label = "race-coordinator";
    return config;
}

Setup prepare(Coordinator& coordinator, std::uint32_t critic_count) {
    Setup setup;
    setup.epoch = coordinator.epoch();

    RegisterTargetRequest target_request;
    target_request.schema_name = "race/target";
    const std::string payload = "race target";
    target_request.payload.assign(payload.begin(), payload.end());
    target_request.epoch = setup.epoch;
    const auto target = coordinator.register_target(target_request);
    if (!target.ok()) {
        std::abort();
    }
    setup.target = target.value();

    ReviewSpec spec = default_review_spec();
    spec.min_critic_count = critic_count;
    spec.quorum = critic_count;
    spec.allow_partial_failure = true;
    spec.duplicates = DuplicatePolicy::KeepDistinct;
    DeclareReviewRequest review_request;
    review_request.request = ReviewRequestId::from_value(1);
    review_request.target = setup.target.id;
    review_request.target_generation = setup.target.generation;
    review_request.spec = spec;
    review_request.epoch = setup.epoch;
    const auto review = coordinator.declare_review(review_request);
    if (!review.ok()) {
        std::abort();
    }
    setup.review = review.value();

    AssignCriticsRequest assign;
    assign.review = setup.review.id;
    assign.review_generation = setup.review.generation;
    assign.epoch = setup.epoch;
    for (std::uint32_t i = 0; i < critic_count; ++i) {
        SyntheticCriticConfig synthetic;
        synthetic.role = CriticRole::GeneralCritic;
        synthetic.behavior = SyntheticBehavior::Pass;
        RegisterCriticRequest critic_request;
        critic_request.capability = synthetic_critic_capability(synthetic);
        critic_request.epoch = setup.epoch;
        const auto critic = coordinator.register_critic(critic_request);
        if (!critic.ok()) {
            std::abort();
        }
        const WorkerBootId boot = generate_worker_boot_id("race-worker");
        RegisterWorkerRequest worker_request;
        worker_request.boot = boot;
        worker_request.critic = critic.value().id;
        worker_request.critic_generation = critic.value().generation;
        worker_request.epoch = setup.epoch;
        const auto worker = coordinator.register_worker(worker_request);
        if (!worker.ok()) {
            std::abort();
        }
        DeclareReadinessRequest readiness;
        readiness.id = worker.value().id;
        readiness.boot = boot;
        readiness.critic = critic.value().id;
        readiness.critic_generation = critic.value().generation;
        readiness.ready = true;
        readiness.healthy = true;
        readiness.capability_evidence = synthetic_capability_evidence(synthetic);
        readiness.epoch = setup.epoch;
        if (!coordinator.declare_readiness(readiness).ok()) {
            std::abort();
        }
        AssignCriticsRequest::Assignment assignment;
        assignment.critic = critic.value().id;
        assignment.critic_generation = critic.value().generation;
        assignment.worker = worker.value().id;
        assignment.worker_boot = boot;
        assignment.role = CriticRole::GeneralCritic;
        assign.assignments.push_back(assignment);
        setup.critics.push_back(Setup::CriticRecord{critic.value().id, critic.value().generation,
                                                    worker.value().id, boot});
    }
    const auto created = coordinator.assign_critics(assign);
    if (!created.ok()) {
        std::abort();
    }
    setup.assignments = created.value();
    setup.template_config = SyntheticCriticConfig{};
    return setup;
}

SubmitEvidenceRequest evidence_request_for(const Setup& setup, std::size_t index, std::uint64_t nonce) {
    const auto& critic = setup.critics[index];
    const auto& assignment = setup.assignments[index];
    SyntheticCriticConfig synthetic;
    synthetic.critic = critic.id;
    synthetic.role = CriticRole::GeneralCritic;
    synthetic.behavior = SyntheticBehavior::Pass;
    const SyntheticReviewOutput output =
        run_synthetic_critic(synthetic, critic.generation, setup.target);

    SubmitEvidenceRequest request;
    request.review = setup.review.id;
    request.review_generation = setup.review.generation;
    request.target = setup.target.id;
    request.target_generation = setup.target.generation;
    request.critic = critic.id;
    request.critic_generation = critic.generation;
    request.worker = critic.worker;
    request.worker_boot = critic.boot;
    request.source_type = output.source_type;
    request.provenance = output.provenance;
    request.integrity = EvidenceIntegrity::Verified;
    request.payload = output.evidence_payload;
    request.epoch = setup.epoch;
    request.request = RequestId::from_value(nonce);
    (void)assignment;
    return request;
}

SubmitFindingRequest finding_request_for(const Setup& setup, std::size_t index, EvidenceId evidence,
                                         std::uint64_t nonce, FindingOutcome outcome) {
    const auto& critic = setup.critics[index];
    SubmitFindingRequest request;
    request.review = setup.review.id;
    request.review_generation = setup.review.generation;
    request.target = setup.target.id;
    request.target_generation = setup.target.generation;
    request.critic = critic.id;
    request.critic_generation = critic.generation;
    request.worker = critic.worker;
    request.worker_boot = critic.boot;
    request.category = FindingCategory::Correctness;
    request.severity = Severity::Critical;
    request.outcome = outcome;
    request.evidence = {evidence};
    request.explanation = "race finding " + std::to_string(index) + " " + std::to_string(nonce);
    request.epoch = setup.epoch;
    request.request = RequestId::from_value(nonce);
    return request;
}

}  // namespace

CF_TEST(two_critics_publishing_simultaneously_both_land_and_aggregate) {
    Coordinator coordinator(make_config());
    CF_REQUIRE(coordinator.start().ok());
    const Setup setup = prepare(coordinator, 2);

    std::vector<EvidenceId> evidence(2);
    {
        Latch latch(2);
        std::vector<Status> statuses(2);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < 2; ++i) {
            threads.emplace_back([&, i] {
                latch.arrive_and_wait();
                const auto result = coordinator.submit_evidence(evidence_request_for(setup, i, 100 + i));
                statuses[i] = result.ok() ? Status::success() : result.status();
                if (result.ok()) {
                    evidence[i] = result.value().id;
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (const Status& status : statuses) {
            CF_CHECK(status.ok());
        }
    }

    {
        Latch latch(2);
        std::vector<Status> statuses(2);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < 2; ++i) {
            threads.emplace_back([&, i] {
                latch.arrive_and_wait();
                const auto result = coordinator.submit_finding(
                    finding_request_for(setup, i, evidence[i], 200 + i, FindingOutcome::Pass));
                statuses[i] = result.ok() ? Status::success() : result.status();
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (const Status& status : statuses) {
            CF_CHECK(status.ok());
        }
    }

    const auto decision = coordinator.commit_review([&] {
        CommitReviewRequest request;
        request.review = setup.review.id;
        request.review_generation = setup.review.generation;
        request.target_generation = setup.target.generation;
        request.epoch = setup.epoch;
        return request;
    }());
    CF_REQUIRE(decision.ok());
    CF_CHECK_EQ(decision.value().disposition, ReviewDisposition::Pass);
    const auto explanation = coordinator.explain_review(setup.review.id);
    CF_REQUIRE(explanation.ok());
    CF_CHECK_EQ(explanation.value().quorum.distinct_decisive_critics, 2u);
}

CF_TEST(concurrent_commit_yields_exactly_one_decision) {
    Coordinator coordinator(make_config());
    CF_REQUIRE(coordinator.start().ok());
    const Setup setup = prepare(coordinator, 2);

    std::vector<EvidenceId> evidence(2);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto result = coordinator.submit_evidence(evidence_request_for(setup, i, 300 + i));
        CF_REQUIRE(result.ok());
        evidence[i] = result.value().id;
        const auto finding = coordinator.submit_finding(
            finding_request_for(setup, i, evidence[i], 400 + i, FindingOutcome::Pass));
        CF_REQUIRE(finding.ok());
    }

    constexpr int kThreads = 4;
    Latch latch(kThreads);
    std::vector<DecisionId> decisions(kThreads);
    std::vector<Status> statuses(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            CommitReviewRequest request;
            request.review = setup.review.id;
            request.review_generation = setup.review.generation;
            request.target_generation = setup.target.generation;
            request.epoch = setup.epoch;
            latch.arrive_and_wait();
            const auto result = coordinator.commit_review(request);
            statuses[static_cast<std::size_t>(i)] = result.ok() ? Status::success() : result.status();
            if (result.ok()) {
                decisions[static_cast<std::size_t>(i)] = result.value().id;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Every competitor either won or observed the already-committed result; no
    // competitor may observe a conflicting commit.
    std::vector<DecisionId> distinct;
    for (int i = 0; i < kThreads; ++i) {
        const Status& status = statuses[static_cast<std::size_t>(i)];
        if (!status.ok()) {
            CF_CHECK(status.code() == ErrorCode::ResultAlreadyCommitted ||
                     status.code() == ErrorCode::ConflictingReviewCommit);
            continue;
        }
        bool seen = false;
        for (DecisionId existing : distinct) {
            if (existing == decisions[static_cast<std::size_t>(i)]) {
                seen = true;
            }
        }
        if (!seen) {
            distinct.push_back(decisions[static_cast<std::size_t>(i)]);
        }
    }
    CF_CHECK_EQ(distinct.size(), 1u);

    const auto snapshot = coordinator.query_review(setup.review.id);
    CF_REQUIRE(snapshot.ok());
    CF_CHECK(snapshot.value().decision.has_value());
    CF_CHECK(snapshot.value().review.state == ReviewState::Committed);
}

CF_TEST(final_finding_races_cancellation_and_the_loser_is_typed) {
    for (int round = 0; round < 32; ++round) {
        Coordinator coordinator(make_config());
        CF_REQUIRE(coordinator.start().ok());
        const Setup setup = prepare(coordinator, 1);
        const auto evidence = coordinator.submit_evidence(evidence_request_for(setup, 0, 500 + round));
        CF_REQUIRE(evidence.ok());

        Latch latch(2);
        Status finding_status;
        Status cancel_status;
        std::thread finding_thread([&] {
            latch.arrive_and_wait();
            const auto result = coordinator.submit_finding(
                finding_request_for(setup, 0, evidence.value().id, 600 + round, FindingOutcome::Pass));
            finding_status = result.ok() ? Status::success() : result.status();
        });
        std::thread cancel_thread([&] {
            CancelReviewRequest request;
            request.review = setup.review.id;
            request.review_generation = setup.review.generation;
            request.reason = "race cancellation";
            request.epoch = setup.epoch;
            latch.arrive_and_wait();
            const auto result = coordinator.cancel_review(request);
            cancel_status = result.ok() ? Status::success() : result.status();
        });
        finding_thread.join();
        cancel_thread.join();

        CF_CHECK(cancel_status.ok());
        const auto snapshot = coordinator.query_review(setup.review.id);
        CF_REQUIRE(snapshot.ok());
        CF_CHECK(snapshot.value().review.state == ReviewState::Cancelled);
        if (!finding_status.ok()) {
            CF_CHECK(finding_status.code() == ErrorCode::ReviewCancelled);
        }

        // Regardless of which side won, cancellation fences the commit.
        const auto commit_attempt = coordinator.commit_review([&] {
            CommitReviewRequest request;
            request.review = setup.review.id;
            request.review_generation = setup.review.generation;
            request.target_generation = setup.target.generation;
            request.epoch = setup.epoch;
            return request;
        }());
        CF_CHECK(!commit_attempt.ok());
        CF_CHECK(commit_attempt.code() == ErrorCode::ReviewCancelled);
    }
}

CF_TEST(final_finding_races_target_supersession) {
    for (int round = 0; round < 32; ++round) {
        Coordinator coordinator(make_config());
        CF_REQUIRE(coordinator.start().ok());
        const Setup setup = prepare(coordinator, 1);
        const auto evidence = coordinator.submit_evidence(evidence_request_for(setup, 0, 700 + round));
        CF_REQUIRE(evidence.ok());

        Latch latch(2);
        Status finding_status;
        Status supersede_status;
        std::thread finding_thread([&] {
            latch.arrive_and_wait();
            const auto result = coordinator.submit_finding(
                finding_request_for(setup, 0, evidence.value().id, 800 + round, FindingOutcome::Pass));
            finding_status = result.ok() ? Status::success() : result.status();
        });
        std::thread supersede_thread([&] {
            RegisterTargetRequest request;
            request.id = setup.target.id;
            request.target_class = setup.target.target_class;
            request.schema_name = setup.target.schema_name;
            const std::string payload = "raced revision " + std::to_string(round);
            request.payload.assign(payload.begin(), payload.end());
            request.epoch = setup.epoch;
            latch.arrive_and_wait();
            const auto result = coordinator.register_target(request);
            supersede_status = result.ok() ? Status::success() : result.status();
        });
        finding_thread.join();
        supersede_thread.join();

        CF_CHECK(supersede_status.ok());
        const auto snapshot = coordinator.query_review(setup.review.id);
        CF_REQUIRE(snapshot.ok());
        CF_CHECK(snapshot.value().review.state == ReviewState::Superseded);
        if (!finding_status.ok()) {
            CF_CHECK(finding_status.code() == ErrorCode::StaleTargetGeneration ||
                     finding_status.code() == ErrorCode::ReviewSuperseded ||
                     finding_status.code() == ErrorCode::TargetMismatch ||
                     finding_status.code() == ErrorCode::TargetSuperseded);
        }
        for (const Finding& finding : snapshot.value().findings) {
            CF_CHECK(finding.state == FindingState::Invalidated ||
                     finding.state == FindingState::Current ||
                     finding.state == FindingState::Superseded);
        }
    }
}

CF_TEST(worker_fencing_races_finding_publication) {
    for (int round = 0; round < 32; ++round) {
        Coordinator coordinator(make_config());
        CF_REQUIRE(coordinator.start().ok());
        const Setup setup = prepare(coordinator, 1);
        const auto evidence = coordinator.submit_evidence(evidence_request_for(setup, 0, 900 + round));
        CF_REQUIRE(evidence.ok());

        Latch latch(2);
        Status finding_status;
        Status fence_status;
        std::thread finding_thread([&] {
            latch.arrive_and_wait();
            const auto result = coordinator.submit_finding(
                finding_request_for(setup, 0, evidence.value().id, 1000 + round, FindingOutcome::Pass));
            finding_status = result.ok() ? Status::success() : result.status();
        });
        std::thread fence_thread([&] {
            latch.arrive_and_wait();
            fence_status = coordinator.fence_worker(setup.critics[0].worker, "raced fence");
        });
        finding_thread.join();
        fence_thread.join();

        CF_CHECK(fence_status.ok());
        const auto snapshot = coordinator.query_review(setup.review.id);
        CF_REQUIRE(snapshot.ok());

        // The fenced incarnation must never publish again, whichever side of the
        // race it lost. This is the invariant the fence exists to guarantee.
        const auto after_fence = coordinator.submit_finding(finding_request_for(
            setup, 0, evidence.value().id, 1500 + static_cast<std::uint64_t>(round), FindingOutcome::Warn));
        CF_CHECK(!after_fence.ok());
        CF_CHECK(after_fence.code() == ErrorCode::CriticNotAuthoritative ||
                 after_fence.code() == ErrorCode::StaleWorkerBoot ||
                 after_fence.code() == ErrorCode::WorkerNotRegistered ||
                 after_fence.code() == ErrorCode::ReviewNotOpen ||
                 after_fence.code() == ErrorCode::ConflictingFinding ||
                 after_fence.code() == ErrorCode::ResultAlreadyCommitted);

        const auto commit_attempt = coordinator.commit_review([&] {
            CommitReviewRequest request;
            request.review = setup.review.id;
            request.review_generation = setup.review.generation;
            request.target_generation = setup.target.generation;
            request.epoch = setup.epoch;
            return request;
        }());

        if (!finding_status.ok()) {
            // The fence won: nothing was published under this review, so the
            // review can never reach an authoritative PASS.
            CF_CHECK(finding_status.code() == ErrorCode::CriticNotAuthoritative ||
                     finding_status.code() == ErrorCode::StaleWorkerBoot ||
                     finding_status.code() == ErrorCode::WorkerNotRegistered ||
                     finding_status.code() == ErrorCode::ReviewNotOpen);
            CF_CHECK(!commit_attempt.ok() ||
                     commit_attempt.value().disposition != ReviewDisposition::Pass);
        } else {
            // The finding won the race. The assignment was discharged before the
            // incarnation left, so the published work stays current; what is lost
            // is only the right to publish more, which the check above proves.
            const bool committed = commit_attempt.ok();
            CF_CHECK(committed);
            if (committed) {
                CF_CHECK(commit_attempt.value().disposition == ReviewDisposition::Pass);
            }
        }
    }
}

CF_TEST(concurrent_readers_never_observe_torn_state) {
    Coordinator coordinator(make_config());
    CF_REQUIRE(coordinator.start().ok());
    const Setup setup = prepare(coordinator, 2);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<bool> torn{false};
    // The writer must not finish before the reader has observed at least one
    // consistent snapshot, so the reader signals its first completed read.
    Latch first_read(2);
    std::thread reader([&] {
        bool signalled = false;
        while (!stop.load()) {
            const auto snapshot = coordinator.query_review(setup.review.id);
            if (!snapshot.ok()) {
                torn.store(true);
                break;
            }
            for (const Finding& finding : snapshot.value().findings) {
                // Every observed finding must carry a complete authority binding.
                if (!finding.critic.valid() || !finding.review.valid() ||
                    finding.review != snapshot.value().review.id) {
                    torn.store(true);
                }
            }
            const auto listed = coordinator.list_reviews();
            if (listed.empty()) {
                torn.store(true);
            }
            reads.fetch_add(1);
            if (!signalled) {
                signalled = true;
                first_read.arrive_and_wait();
            }
        }
        if (!signalled) {
            first_read.arrive_and_wait();
        }
    });
    first_read.arrive_and_wait();

    for (std::size_t i = 0; i < 2; ++i) {
        const auto evidence = coordinator.submit_evidence(evidence_request_for(setup, i, 2000 + i));
        CF_REQUIRE(evidence.ok());
        const auto finding = coordinator.submit_finding(
            finding_request_for(setup, i, evidence.value().id, 2100 + i, FindingOutcome::Pass));
        CF_REQUIRE(finding.ok());
    }
    const auto decision = coordinator.commit_review([&] {
        CommitReviewRequest request;
        request.review = setup.review.id;
        request.review_generation = setup.review.generation;
        request.target_generation = setup.target.generation;
        request.epoch = setup.epoch;
        return request;
    }());
    CF_REQUIRE(decision.ok());

    stop.store(true);
    reader.join();
    CF_CHECK(!torn.load());
    CF_CHECK(reads.load() > 0);
}

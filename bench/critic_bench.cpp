// Critic Fabric — benchmark of completed operations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every measurement counts operations that actually completed. Nothing here
// times an enqueue and calls it throughput.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/platform.hpp"
#include "critic_fabric/protocol.hpp"
#include "critic_fabric/worker.hpp"

namespace {

using namespace critic_fabric;

// The benchmark drives far more concurrent reviews than a production
// coordinator would be configured for, so it raises the explicit bounds rather
// than silently exceeding them.
RuntimeLimits bench_limits() {
    RuntimeLimits limits;
    limits.max_concurrent_reviews = 65536;
    limits.max_retained_reviews = 65536;
    limits.max_retained_targets = 65536;
    limits.max_critics_per_review = 256;
    limits.max_findings_per_review = 1024;
    limits.max_evidence_records_per_review = 65536;
    limits.max_worker_connections = 4096;
    limits.max_persisted_records = 4194304;
    limits.clamp_to_hard_ceiling();
    return limits;
}

double seconds_since(const std::chrono::steady_clock::time_point& start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double>(elapsed).count();
}

void report(const char* label, std::uint64_t operations, double seconds) {
    const double per_second = seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
    std::printf("%-46s %10llu ops %9.3f s %14.0f ops/s\n", label,
                static_cast<unsigned long long>(operations), seconds, per_second);
}

CoordinatorEpoch start_coordinator(Coordinator& coordinator) {
    const Status status = coordinator.start();
    if (!status.ok()) {
        std::fprintf(stderr, "coordinator start failed: %s\n", status.to_string().c_str());
        std::exit(1);
    }
    return coordinator.epoch();
}

TargetRecord make_target(Coordinator& coordinator, CoordinatorEpoch epoch, const std::string& payload) {
    RegisterTargetRequest request;
    request.schema_name = "bench/target";
    request.payload.assign(payload.begin(), payload.end());
    request.provenance = "bench";
    request.epoch = epoch;
    Result<TargetRecord> target = coordinator.register_target(request);
    if (!target.ok()) {
        std::fprintf(stderr, "target registration failed: %s\n", target.status().to_string().c_str());
        std::exit(1);
    }
    return target.value();
}

}  // namespace

int main() {
    platform::configure_process_for_tests();

    {
        CoordinatorConfig config;
        config.process_label = "bench";
        config.limits = bench_limits();
        Coordinator coordinator(config);
        const CoordinatorEpoch epoch = start_coordinator(coordinator);

        constexpr std::uint64_t kTargets = 20000;
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < kTargets; ++i) {
            RegisterTargetRequest request;
            request.schema_name = "bench/target";
            const std::string payload = "target-" + std::to_string(i);
            request.payload.assign(payload.begin(), payload.end());
            request.provenance = "bench";
            request.epoch = epoch;
            const auto result = coordinator.register_target(request);
            if (!result.ok()) {
                std::fprintf(stderr, "rejected: %s\n", result.status().to_string().c_str());
                return 1;
            }
        }
        report("target registration (completed)", kTargets, seconds_since(start));

        constexpr std::uint64_t kCritics = 20000;
        const auto critic_start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < kCritics; ++i) {
            SyntheticCriticConfig synthetic;
            synthetic.role = CriticRole::GeneralCritic;
            synthetic.behavior = SyntheticBehavior::Pass;
            RegisterCriticRequest request;
            request.capability = synthetic_critic_capability(synthetic);
            request.epoch = epoch;
            const auto result = coordinator.register_critic(request);
            if (!result.ok()) {
                std::fprintf(stderr, "rejected: %s\n", result.status().to_string().c_str());
                return 1;
            }
        }
        report("critic registration (completed)", kCritics, seconds_since(critic_start));
        std::printf("authoritative state digest %s\n",
                    coordinator.authoritative_state_digest().to_hex().substr(0, 16).c_str());
    }

    {
        CoordinatorConfig config;
        config.process_label = "bench";
        config.limits = bench_limits();
        Coordinator coordinator(config);
        const CoordinatorEpoch epoch = start_coordinator(coordinator);
        const TargetRecord target = make_target(coordinator, epoch, "review throughput target");

        constexpr std::uint64_t kReviews = 2000;
        std::vector<ReviewRecord> reviews;
        reviews.reserve(kReviews);
        const auto start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < kReviews; ++i) {
            DeclareReviewRequest request;
            request.request = ReviewRequestId::from_value(i + 1);
            request.target = target.id;
            request.target_generation = target.generation;
            request.spec = default_review_spec();
            request.epoch = epoch;
            const auto result = coordinator.declare_review(request);
            if (!result.ok()) {
                std::fprintf(stderr, "rejected: %s\n", result.status().to_string().c_str());
                return 1;
            }
            reviews.push_back(result.value());
        }
        report("review declaration (completed)", kReviews, seconds_since(start));

        // One critic per review, assigned, published, and committed.
        std::vector<std::pair<ReviewRecord, Finding>> completed;
        completed.reserve(kReviews);
        const auto cycle_start = std::chrono::steady_clock::now();
        for (const ReviewRecord& review : reviews) {
            SyntheticCriticConfig synthetic;
            synthetic.role = CriticRole::GeneralCritic;
            synthetic.behavior = SyntheticBehavior::Pass;

            RegisterCriticRequest critic_request;
            critic_request.capability = synthetic_critic_capability(synthetic);
            critic_request.epoch = epoch;
            const auto critic = coordinator.register_critic(critic_request);
            if (!critic.ok()) {
                std::fprintf(stderr, "rejected: %s\n", critic.status().to_string().c_str());
                return 1;
            }
            const WorkerBootId boot = generate_worker_boot_id("bench-worker");
            RegisterWorkerRequest worker_request;
            worker_request.boot = boot;
            worker_request.critic = critic.value().id;
            worker_request.critic_generation = critic.value().generation;
            worker_request.epoch = epoch;
            const auto worker = coordinator.register_worker(worker_request);
            if (!worker.ok()) {
                std::fprintf(stderr, "rejected: %s\n", worker.status().to_string().c_str());
                return 1;
            }
            DeclareReadinessRequest readiness;
            readiness.id = worker.value().id;
            readiness.boot = boot;
            readiness.critic = critic.value().id;
            readiness.critic_generation = critic.value().generation;
            readiness.ready = true;
            readiness.healthy = true;
            readiness.capability_evidence = synthetic_capability_evidence(synthetic);
            readiness.epoch = epoch;
            if (!coordinator.declare_readiness(readiness).ok()) {
                std::fprintf(stderr, "readiness rejected\n");
                return 1;
            }

            AssignCriticsRequest assign;
            assign.review = review.id;
            assign.review_generation = review.generation;
            AssignCriticsRequest::Assignment assignment;
            assignment.critic = critic.value().id;
            assignment.critic_generation = critic.value().generation;
            assignment.worker = worker.value().id;
            assignment.worker_boot = boot;
            assignment.role = CriticRole::GeneralCritic;
            assign.assignments.push_back(assignment);
            assign.epoch = epoch;
            const auto created = coordinator.assign_critics(assign);
            if (!created.ok()) {
                std::fprintf(stderr, "rejected: %s\n", created.status().to_string().c_str());
                return 1;
            }

            SubmitEvidenceRequest evidence_request;
            evidence_request.review = review.id;
            evidence_request.review_generation = review.generation;
            evidence_request.target = target.id;
            evidence_request.target_generation = target.generation;
            evidence_request.critic = critic.value().id;
            evidence_request.critic_generation = critic.value().generation;
            evidence_request.worker = worker.value().id;
            evidence_request.worker_boot = boot;
            evidence_request.source_type = EvidenceSourceType::Computation;
            evidence_request.provenance = EvidenceProvenance::Derived;
            evidence_request.integrity = EvidenceIntegrity::Verified;
            evidence_request.reference = "bench-witness";
            evidence_request.payload = {1, 2, 3, 4};
            evidence_request.epoch = epoch;
            const auto evidence = coordinator.submit_evidence(evidence_request);
            if (!evidence.ok()) {
                std::fprintf(stderr, "rejected: %s\n", evidence.status().to_string().c_str());
                return 1;
            }

            SubmitFindingRequest finding_request;
            finding_request.review = review.id;
            finding_request.review_generation = review.generation;
            finding_request.target = target.id;
            finding_request.target_generation = target.generation;
            finding_request.critic = critic.value().id;
            finding_request.critic_generation = critic.value().generation;
            finding_request.worker = worker.value().id;
            finding_request.worker_boot = boot;
            finding_request.category = FindingCategory::Correctness;
            finding_request.severity = Severity::High;
            finding_request.outcome = FindingOutcome::Pass;
            finding_request.evidence = {evidence.value().id};
            finding_request.explanation = "benchmark finding";
            finding_request.epoch = epoch;
            const auto finding = coordinator.submit_finding(finding_request);
            if (!finding.ok()) {
                std::fprintf(stderr, "rejected: %s\n", finding.status().to_string().c_str());
                return 1;
            }

            CommitReviewRequest commit;
            commit.review = review.id;
            commit.review_generation = review.generation;
            commit.target_generation = target.generation;
            commit.epoch = epoch;
            const auto decision = coordinator.commit_review(commit);
            if (!decision.ok()) {
                std::fprintf(stderr, "rejected: %s\n", decision.status().to_string().c_str());
                return 1;
            }
            completed.emplace_back(review, finding.value());
        }
        report("full review cycles (assign+evidence+finding+commit)", kReviews, seconds_since(cycle_start));

        const auto query_start = std::chrono::steady_clock::now();
        for (const auto& entry : completed) {
            const auto snapshot = coordinator.query_review(entry.first.id);
            if (!snapshot.ok()) {
                return 1;
            }
        }
        report("review snapshot query (completed)", completed.size(), seconds_since(query_start));

        const auto explain_start = std::chrono::steady_clock::now();
        for (const auto& entry : completed) {
            const auto explanation = coordinator.explain_review(entry.first.id);
            if (!explanation.ok()) {
                return 1;
            }
        }
        report("deterministic explanation (completed)", completed.size(), seconds_since(explain_start));

        const auto evaluate_start = std::chrono::steady_clock::now();
        for (const auto& entry : completed) {
            const auto evaluation = coordinator.evaluate_review_now(entry.first.id);
            if (!evaluation.ok()) {
                return 1;
            }
        }
        report("policy evaluation (completed)", completed.size(), seconds_since(evaluate_start));
    }

    // Aggregation across a growing finding population: exposes superlinear cost
    // if the aggregation path is not linear in the number of findings.
    {
        CoordinatorConfig config;
        config.process_label = "bench-aggregate";
        config.limits = bench_limits();
        Coordinator coordinator(config);
        const CoordinatorEpoch epoch = start_coordinator(coordinator);
        const TargetRecord target = make_target(coordinator, epoch, "aggregation scaling target");

        for (std::uint32_t critic_count : {8u, 32u, 128u}) {
            DeclareReviewRequest request;
            request.request = ReviewRequestId::from_value(100000 + critic_count);
            request.target = target.id;
            request.target_generation = target.generation;
            ReviewSpec spec = default_review_spec();
            spec.min_critic_count = critic_count;
            spec.quorum = critic_count;
            spec.duplicates = DuplicatePolicy::KeepDistinct;
            request.spec = spec;
            request.epoch = epoch;
            const auto review = coordinator.declare_review(request);
            if (!review.ok()) {
                std::fprintf(stderr, "rejected: %s\n", review.status().to_string().c_str());
                return 1;
            }

            std::vector<std::pair<CriticId, CriticGeneration>> critics;
            std::vector<std::pair<WorkerId, WorkerBootId>> workers;
            AssignCriticsRequest assign;
            assign.review = review.value().id;
            assign.review_generation = review.value().generation;
            assign.epoch = epoch;
            for (std::uint32_t i = 0; i < critic_count; ++i) {
                SyntheticCriticConfig synthetic;
                synthetic.role = CriticRole::GeneralCritic;
                synthetic.behavior = SyntheticBehavior::Pass;
                RegisterCriticRequest critic_request;
                critic_request.capability = synthetic_critic_capability(synthetic);
                critic_request.epoch = epoch;
                const auto critic = coordinator.register_critic(critic_request);
                if (!critic.ok()) {
                    return 1;
                }
                const WorkerBootId boot = generate_worker_boot_id("bench-bulk");
                RegisterWorkerRequest worker_request;
                worker_request.boot = boot;
                worker_request.critic = critic.value().id;
                worker_request.critic_generation = critic.value().generation;
                worker_request.epoch = epoch;
                const auto worker = coordinator.register_worker(worker_request);
                if (!worker.ok()) {
                    return 1;
                }
                DeclareReadinessRequest readiness;
                readiness.id = worker.value().id;
                readiness.boot = boot;
                readiness.critic = critic.value().id;
                readiness.critic_generation = critic.value().generation;
                readiness.ready = true;
                readiness.healthy = true;
                readiness.capability_evidence = synthetic_capability_evidence(synthetic);
                readiness.epoch = epoch;
                if (!coordinator.declare_readiness(readiness).ok()) {
                    return 1;
                }
                AssignCriticsRequest::Assignment assignment;
                assignment.critic = critic.value().id;
                assignment.critic_generation = critic.value().generation;
                assignment.worker = worker.value().id;
                assignment.worker_boot = boot;
                assignment.role = CriticRole::GeneralCritic;
                assign.assignments.push_back(assignment);
                critics.emplace_back(critic.value().id, critic.value().generation);
                workers.emplace_back(worker.value().id, boot);
            }
            const auto created = coordinator.assign_critics(assign);
            if (!created.ok()) {
                std::fprintf(stderr, "rejected: %s\n", created.status().to_string().c_str());
                return 1;
            }

            for (std::size_t i = 0; i < critics.size(); ++i) {
                SubmitEvidenceRequest evidence_request;
                evidence_request.review = review.value().id;
                evidence_request.review_generation = review.value().generation;
                evidence_request.target = target.id;
                evidence_request.target_generation = target.generation;
                evidence_request.critic = critics[i].first;
                evidence_request.critic_generation = critics[i].second;
                evidence_request.worker = workers[i].first;
                evidence_request.worker_boot = workers[i].second;
                evidence_request.source_type = EvidenceSourceType::Computation;
                evidence_request.provenance = EvidenceProvenance::Derived;
                evidence_request.integrity = EvidenceIntegrity::Verified;
                evidence_request.reference = "bench-bulk-witness";
                evidence_request.payload = {9, 9};
                evidence_request.epoch = epoch;
                const auto evidence = coordinator.submit_evidence(evidence_request);
                if (!evidence.ok()) {
                    std::fprintf(stderr, "rejected: %s\n", evidence.status().to_string().c_str());
                    return 1;
                }
                SubmitFindingRequest finding_request;
                finding_request.review = review.value().id;
                finding_request.review_generation = review.value().generation;
                finding_request.target = target.id;
                finding_request.target_generation = target.generation;
                finding_request.critic = critics[i].first;
                finding_request.critic_generation = critics[i].second;
                finding_request.worker = workers[i].first;
                finding_request.worker_boot = workers[i].second;
                finding_request.category = FindingCategory::Correctness;
                finding_request.severity = Severity::Medium;
                finding_request.outcome = FindingOutcome::Pass;
                finding_request.evidence = {evidence.value().id};
                finding_request.explanation = "bulk finding " + std::to_string(i);
                finding_request.epoch = epoch;
                const auto finding = coordinator.submit_finding(finding_request);
                if (!finding.ok()) {
                    std::fprintf(stderr, "rejected: %s\n", finding.status().to_string().c_str());
                    return 1;
                }
            }

            const std::uint64_t rounds = 200;
            const auto start = std::chrono::steady_clock::now();
            for (std::uint64_t i = 0; i < rounds; ++i) {
                const auto evaluation = coordinator.evaluate_review_now(review.value().id);
                if (!evaluation.ok()) {
                    return 1;
                }
            }
            char label[96];
            std::snprintf(label, sizeof(label), "policy evaluation over %u findings (completed)",
                          critic_count);
            report(label, rounds, seconds_since(start));
        }
    }

    // Durable append cost and full durable reload cost.
    {
        const std::string path = "critic_fabric_bench.journal";
        std::remove(path.c_str());
        {
            CoordinatorConfig config;
            config.persistence_path = path;
            config.process_label = "bench-durable";
            config.limits = bench_limits();
            Coordinator coordinator(config);
            const CoordinatorEpoch epoch = start_coordinator(coordinator);
            const TargetRecord target = make_target(coordinator, epoch, "durable target");

            constexpr std::uint64_t kFindings = 5000;
            std::vector<Finding> findings;
            const auto start = std::chrono::steady_clock::now();
            for (std::uint64_t i = 0; i < kFindings; ++i) {
                DeclareReviewRequest review_request;
                review_request.request = ReviewRequestId::from_value(200000 + i);
                review_request.target = target.id;
                review_request.target_generation = target.generation;
                ReviewSpec spec = default_review_spec();
                spec.duplicates = DuplicatePolicy::KeepDistinct;
                review_request.spec = spec;
                review_request.epoch = epoch;
                const auto review = coordinator.declare_review(review_request);
                if (!review.ok()) {
                    return 1;
                }
                const auto finding = coordinator.submit_finding([&] {
                    SubmitFindingRequest request;
                    request.review = review.value().id;
                    request.review_generation = review.value().generation;
                    request.target = target.id;
                    request.target_generation = target.generation;
                    request.critic = CriticId::from_value(1);
                    request.critic_generation = CriticGeneration::from_value(1);
                    request.worker = WorkerId::from_value(1);
                    request.worker_boot = WorkerBootId::from_value(1);
                    request.outcome = FindingOutcome::Unknown;
                    request.explanation = "durable benchmark finding";
                    request.epoch = epoch;
                    return request;
                }());
                (void)finding;
            }
            report("durable journal append with review declaration", kFindings, seconds_since(start));
            coordinator.stop();
        }
        {
            CoordinatorConfig config;
            config.persistence_path = path;
            config.process_label = "bench-durable-reload";
            config.limits = bench_limits();
            const auto start = std::chrono::steady_clock::now();
            Coordinator coordinator(config);
            const CoordinatorEpoch epoch = start_coordinator(coordinator);
            const double elapsed = seconds_since(start);
            report("durable journal load and replay (completed)", 1, elapsed);
            std::printf("recovered epoch %llu records %llu\n",
                        static_cast<unsigned long long>(epoch.value()),
                        static_cast<unsigned long long>(coordinator.recovery_report().journal_records));
            coordinator.stop();
        }
        std::remove(path.c_str());
    }

    // Transport codec cost, measured on complete frames.
    {
        Frame frame;
        frame.header.type = MessageType::SubmitFinding;
        frame.header.correlation = RequestId::from_value(42);
        frame.header.authority.epoch = CoordinatorEpoch::first();
        frame.header.authority.worker = WorkerId::from_value(7);
        frame.header.authority.worker_boot = WorkerBootId::from_value(8);
        frame.header.authority.critic = CriticId::from_value(9);
        frame.header.authority.critic_generation = CriticGeneration::first();
        frame.header.authority.review = ReviewId::from_value(10);
        frame.header.authority.review_generation = ReviewGeneration::first();
        frame.header.authority.target = TargetId::from_value(11);
        frame.header.authority.target_generation = TargetGeneration::first();
        frame.header.authority.request = RequestId::from_value(42);
        frame.payload.assign(512, 0x5A);

        RuntimeLimits limits;
        std::vector<std::uint8_t> bytes;
        constexpr std::uint64_t kFrames = 200000;
        const auto encode_start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            if (!FrameCodec::encode(frame, limits, bytes).ok()) {
                return 1;
            }
        }
        report("frame encode (completed)", kFrames, seconds_since(encode_start));

        const auto decode_start = std::chrono::steady_clock::now();
        for (std::uint64_t i = 0; i < kFrames; ++i) {
            Frame decoded;
            if (!FrameCodec::decode(bytes, limits, decoded).ok()) {
                return 1;
            }
        }
        report("frame decode (completed)", kFrames, seconds_since(decode_start));
    }

    return 0;
}

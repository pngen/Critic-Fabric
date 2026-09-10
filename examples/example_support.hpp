// Critic Fabric — shared helpers for the runnable examples.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_EXAMPLE_SUPPORT_HPP
#define CRITIC_FABRIC_EXAMPLE_SUPPORT_HPP

#include <cstdio>
#include <string>
#include <vector>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/worker.hpp"

namespace example {

using namespace critic_fabric;

struct Harness {
    Coordinator coordinator;
    std::vector<unsigned char> log;

    explicit Harness(bool durable, const std::string& path)
        : coordinator(make_config(durable, path)) {
        const Status status = coordinator.start();
        if (!status.ok()) {
            std::fprintf(stderr, "coordinator start failed: %s\n", status.to_string().c_str());
            std::exit(1);
        }
    }

    static CoordinatorConfig make_config(bool durable, const std::string& path) {
        CoordinatorConfig config;
        if (durable) {
            config.persistence_path = path;
        }
        config.process_label = "example-coordinator";
        return config;
    }

    [[nodiscard]] CoordinatorEpoch epoch() const { return coordinator.epoch(); }

    TargetRecord make_target(const std::string& payload, TargetClass target_class = TargetClass::Claim) {
        RegisterTargetRequest request;
        request.target_class = target_class;
        request.schema_name = "example/claim";
        request.payload.assign(payload.begin(), payload.end());
        request.provenance = "example-harness";
        request.epoch = epoch();
        Result<TargetRecord> target = coordinator.register_target(request);
        require(target.ok(), target.status());
        return target.value();
    }

    struct Critic {
        CriticId id{};
        CriticGeneration generation{};
        WorkerId worker{};
        WorkerBootId boot{};
    };

    Critic make_critic(CriticRole role, SyntheticBehavior behavior,
                       FindingCategory category = FindingCategory::Correctness) {
        SyntheticCriticConfig synthetic;
        synthetic.role = role;
        synthetic.behavior = behavior;
        synthetic.category = category;

        RegisterCriticRequest critic_request;
        critic_request.capability = synthetic_critic_capability(synthetic);
        critic_request.provenance = "example-harness";
        critic_request.epoch = epoch();
        Result<CriticRegistration> critic = coordinator.register_critic(critic_request);
        require(critic.ok(), critic.status());

        Critic result;
        result.id = critic.value().id;
        result.generation = critic.value().generation;
        result.boot = generate_worker_boot_id("example-worker");

        RegisterWorkerRequest worker_request;
        worker_request.boot = result.boot;
        worker_request.critic = result.id;
        worker_request.critic_generation = result.generation;
        worker_request.process_label = "example-worker";
        worker_request.epoch = epoch();
        Result<WorkerRecord> worker = coordinator.register_worker(worker_request);
        require(worker.ok(), worker.status());
        result.worker = worker.value().id;

        DeclareReadinessRequest readiness;
        readiness.id = result.worker;
        readiness.boot = result.boot;
        readiness.critic = result.id;
        readiness.critic_generation = result.generation;
        readiness.ready = true;
        readiness.healthy = true;
        readiness.capability_evidence = synthetic_capability_evidence(synthetic);
        readiness.epoch = epoch();
        require(coordinator.declare_readiness(readiness).ok(), Status::error(ErrorCode::InternalError, "readiness"));
        return result;
    }

    std::vector<CriticAssignment> assign(ReviewId review, ReviewGeneration generation,
                                         const std::vector<std::pair<Critic, CriticRole>>& critics) {
        AssignCriticsRequest request;
        request.review = review;
        request.review_generation = generation;
        for (const auto& entry : critics) {
            AssignCriticsRequest::Assignment assignment;
            assignment.critic = entry.first.id;
            assignment.critic_generation = entry.first.generation;
            assignment.worker = entry.first.worker;
            assignment.worker_boot = entry.first.boot;
            assignment.role = entry.second;
            request.assignments.push_back(assignment);
        }
        request.epoch = epoch();
        Result<std::vector<CriticAssignment>> created = coordinator.assign_critics(request);
        require(created.ok(), created.status());
        return created.value();
    }

    Finding publish(const CriticAssignment& assignment, const TargetRecord& target,
                    const Critic& critic, SyntheticBehavior behavior,
                    const std::string& explanation = "synthetic reference assessment") {
        SyntheticCriticConfig synthetic;
        synthetic.critic = critic.id;
        synthetic.behavior = behavior;
        const SyntheticReviewOutput output =
            run_synthetic_critic(synthetic, critic.generation, target);

        SubmitEvidenceRequest evidence_request;
        evidence_request.review = assignment.review;
        evidence_request.review_generation = assignment.review_generation;
        evidence_request.target = assignment.target;
        evidence_request.target_generation = assignment.target_generation;
        evidence_request.critic = critic.id;
        evidence_request.critic_generation = critic.generation;
        evidence_request.worker = critic.worker;
        evidence_request.worker_boot = critic.boot;
        evidence_request.source_type = output.source_type;
        evidence_request.provenance = output.provenance;
        evidence_request.integrity = EvidenceIntegrity::Verified;
        evidence_request.reference = "example-witness";
        evidence_request.payload = output.evidence_payload;
        evidence_request.epoch = epoch();
        evidence_request.request = RequestId::from_value(assignment.id.value() * 2u);
        Result<EvidenceRecord> evidence = coordinator.submit_evidence(evidence_request);
        require(evidence.ok(), evidence.status());

        SubmitFindingRequest finding_request;
        finding_request.review = assignment.review;
        finding_request.review_generation = assignment.review_generation;
        finding_request.target = assignment.target;
        finding_request.target_generation = assignment.target_generation;
        finding_request.critic = critic.id;
        finding_request.critic_generation = critic.generation;
        finding_request.worker = critic.worker;
        finding_request.worker_boot = critic.boot;
        finding_request.category = output.category;
        finding_request.severity = output.severity;
        finding_request.outcome = output.outcome;
        finding_request.evidence = {evidence.value().id};
        finding_request.explanation = explanation;
        finding_request.provenance = "example-harness";
        finding_request.epoch = epoch();
        finding_request.request = RequestId::from_value(assignment.id.value() * 2u + 1u);
        Result<Finding> finding = coordinator.submit_finding(finding_request);
        require(finding.ok(), finding.status());
        return finding.value();
    }

    ReviewRecord declare(const TargetRecord& target, const ReviewSpec& spec) {
        static std::uint64_t counter = 1;
        DeclareReviewRequest request;
        request.request = ReviewReviewRequestId(counter++);
        request.target = target.id;
        request.target_generation = target.generation;
        request.spec = spec;
        request.epoch = epoch();
        Result<ReviewRecord> review = coordinator.declare_review(request);
        require(review.ok(), review.status());
        return review.value();
    }

    Decision commit(const ReviewRecord& review) {
        CommitReviewRequest request;
        request.review = review.id;
        request.review_generation = review.generation;
        request.target_generation = review.target_generation;
        request.epoch = epoch();
        Result<Decision> decision = coordinator.commit_review(request);
        require(decision.ok(), decision.status());
        return decision.value();
    }

    static ReviewRequestId ReviewReviewRequestId(std::uint64_t value) {
        return ReviewRequestId::from_value(value);
    }

    static void require(bool condition, const Status& status) {
        if (!condition) {
            std::fprintf(stderr, "example precondition failed: %s\n", status.to_string().c_str());
            std::exit(1);
        }
    }

    static void require(bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "example precondition failed: %s\n", what);
            std::exit(1);
        }
    }
};

inline void banner(const char* title) {
    std::printf("=== %s ===\n", title);
}

inline void show_decision(const Decision& decision, const std::string& label) {
    std::printf("%s: disposition=%s digest=%s\n", label.c_str(), to_string(decision.disposition),
                decision.decision_digest.to_hex().substr(0, 16).c_str());
}

}  // namespace example

#endif  // CRITIC_FABRIC_EXAMPLE_SUPPORT_HPP

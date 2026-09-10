// Critic Fabric — shared test fixture over the in-process coordinator.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_TEST_FIXTURE_HPP
#define CRITIC_FABRIC_TEST_FIXTURE_HPP

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/worker.hpp"
#include "test_support.hpp"

namespace cftest {

using namespace critic_fabric;

struct Fixture {
    Coordinator coordinator;

    explicit Fixture(bool durable = false, const std::string& path = std::string())
        : coordinator(make_config(durable, path)) {
        const Status status = coordinator.start();
        if (!status.ok()) {
            std::fprintf(stderr, "fixture coordinator start failed: %s\n", status.to_string().c_str());
            std::abort();
        }
    }

    static CoordinatorConfig make_config(bool durable, const std::string& path) {
        CoordinatorConfig config;
        if (durable) {
            config.persistence_path = path;
        }
        config.process_label = "test-fixture";
        return config;
    }

    [[nodiscard]] CoordinatorEpoch epoch() const { return coordinator.epoch(); }

    TargetRecord target(const std::string& payload, TargetClass target_class = TargetClass::Claim) {
        RegisterTargetRequest request;
        request.target_class = target_class;
        request.schema_name = "test/target";
        request.payload.assign(payload.begin(), payload.end());
        request.provenance = "test-fixture";
        request.epoch = epoch();
        const auto result = coordinator.register_target(request);
        CF_REQUIRE(result.ok());
        return result.value();
    }

    TargetRecord advance_target(const TargetRecord& previous, const std::string& payload) {
        RegisterTargetRequest request;
        request.id = previous.id;
        request.target_class = previous.target_class;
        request.schema_name = previous.schema_name;
        request.payload.assign(payload.begin(), payload.end());
        request.provenance = "test-fixture";
        request.epoch = epoch();
        const auto result = coordinator.register_target(request);
        CF_REQUIRE(result.ok());
        return result.value();
    }

    struct Critic {
        CriticId id{};
        CriticGeneration generation{};
        WorkerId worker{};
        WorkerBootId boot{};
        SyntheticCriticConfig config;

        [[nodiscard]] AuthorityContext authority(ReviewId review, ReviewGeneration generation_value,
                                                 TargetId target_id,
                                                 TargetGeneration target_generation) const {
            AuthorityContext context;
            context.epoch = CoordinatorEpoch{};
            context.worker = worker;
            context.worker_boot = boot;
            context.critic = id;
            context.critic_generation = generation;
            context.review = review;
            context.review_generation = generation_value;
            context.target = target_id;
            context.target_generation = target_generation;
            return context;
        }
    };

    Critic critic(CriticRole role, SyntheticBehavior behavior,
                  FindingCategory category = FindingCategory::Correctness,
                  Severity severity = Severity::Critical, bool ready = true) {
        SyntheticCriticConfig synthetic;
        synthetic.role = role;
        synthetic.behavior = behavior;
        synthetic.category = category;
        synthetic.severity = severity;

        RegisterCriticRequest critic_request;
        critic_request.capability = synthetic_critic_capability(synthetic);
        critic_request.provenance = "test-fixture";
        critic_request.epoch = epoch();
        const auto registration = coordinator.register_critic(critic_request);
        CF_REQUIRE(registration.ok());
        return finish_critic(registration.value(), synthetic, ready);
    }

    Critic advance_critic(const Critic& previous) {
        SyntheticCriticConfig synthetic = previous.config;
        synthetic.implementation_version = synthetic.implementation_version + "-next";
        RegisterCriticRequest request;
        request.id = previous.id;
        request.capability = synthetic_critic_capability(synthetic);
        request.advance_generation = true;
        request.epoch = epoch();
        const auto registration = coordinator.register_critic(request);
        CF_REQUIRE(registration.ok());
        return finish_critic(registration.value(), synthetic, true);
    }

    Critic finish_critic(const CriticRegistration& registration, const SyntheticCriticConfig& synthetic,
                         bool ready) {
        Critic result;
        result.id = registration.id;
        result.generation = registration.generation;
        result.config = synthetic;
        result.config.critic = registration.id;
        result.boot = generate_worker_boot_id("test-worker");
        RegisterWorkerRequest worker_request;
        worker_request.boot = result.boot;
        worker_request.critic = registration.id;
        worker_request.critic_generation = registration.generation;
        worker_request.process_label = "test-worker";
        worker_request.epoch = epoch();
        const auto worker = coordinator.register_worker(worker_request);
        CF_REQUIRE(worker.ok());
        result.worker = worker.value().id;
        if (ready) {
            DeclareReadinessRequest readiness;
            readiness.id = result.worker;
            readiness.boot = result.boot;
            readiness.critic = result.id;
            readiness.critic_generation = result.generation;
            readiness.ready = true;
            readiness.healthy = true;
            readiness.capability_evidence = synthetic_capability_evidence(synthetic);
            readiness.epoch = epoch();
            const auto declared = coordinator.declare_readiness(readiness);
            CF_REQUIRE(declared.ok());
        }
        return result;
    }

    ReviewRecord declare(const TargetRecord& target_record, const ReviewSpec& spec) {
        const std::uint64_t request_id = next_request_++;
        DeclareReviewRequest request;
        request.request = ReviewRequestId::from_value(request_id);
        request.target = target_record.id;
        request.target_generation = target_record.generation;
        request.spec = spec;
        request.epoch = epoch();
        const auto review = coordinator.declare_review(request);
        CF_REQUIRE(review.ok());
        return review.value();
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
        const auto created = coordinator.assign_critics(request);
        CF_REQUIRE(created.ok());
        return created.value();
    }

    Result<std::vector<CriticAssignment>> try_assign(
        ReviewId review, ReviewGeneration generation,
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
        return coordinator.assign_critics(request);
    }

    Result<EvidenceRecord> try_evidence(const CriticAssignment& assignment, const TargetRecord& target_record,
                                        const Critic& critic_record, std::uint64_t nonce) {
        const SyntheticReviewOutput output =
            run_synthetic_critic(critic_record.config, critic_record.generation, target_record);
        SubmitEvidenceRequest request;
        request.review = assignment.review;
        request.review_generation = assignment.review_generation;
        request.target = assignment.target;
        request.target_generation = assignment.target_generation;
        request.critic = critic_record.id;
        request.critic_generation = critic_record.generation;
        request.worker = critic_record.worker;
        request.worker_boot = critic_record.boot;
        request.source_type = output.source_type;
        request.provenance = output.provenance;
        request.integrity = EvidenceIntegrity::Verified;
        request.reference = "test-witness";
        request.payload = output.evidence_payload;
        request.epoch = epoch();
        request.request = RequestId::from_value(nonce);
        return coordinator.submit_evidence(request);
    }

    EvidenceRecord evidence(const CriticAssignment& assignment, const TargetRecord& target_record,
                            const Critic& critic_record, std::uint64_t nonce) {
        const auto result = try_evidence(assignment, target_record, critic_record, nonce);
        CF_REQUIRE(result.ok());
        return result.value();
    }

    Result<Finding> try_publish(const CriticAssignment& assignment, const TargetRecord& target_record,
                                const Critic& critic_record, SyntheticBehavior behavior,
                                std::uint64_t nonce, const std::string& explanation = "test finding") {
        SyntheticCriticConfig configuration = critic_record.config;
        configuration.behavior = behavior;
        const SyntheticReviewOutput output =
            run_synthetic_critic(configuration, critic_record.generation, target_record);
        const auto evidence_result = try_evidence(assignment, target_record, critic_record, nonce * 2 + 1);
        if (!evidence_result.ok()) {
            return evidence_result.status();
        }
        SubmitFindingRequest request;
        request.review = assignment.review;
        request.review_generation = assignment.review_generation;
        request.target = assignment.target;
        request.target_generation = assignment.target_generation;
        request.critic = critic_record.id;
        request.critic_generation = critic_record.generation;
        request.worker = critic_record.worker;
        request.worker_boot = critic_record.boot;
        request.category = output.category;
        request.severity = output.severity;
        request.outcome = output.outcome;
        request.evidence = {evidence_result.value().id};
        request.explanation = explanation;
        request.provenance = "test-fixture";
        request.epoch = epoch();
        request.request = RequestId::from_value(nonce * 2 + 2);
        return coordinator.submit_finding(request);
    }

    Finding publish(const CriticAssignment& assignment, const TargetRecord& target_record,
                    const Critic& critic_record, SyntheticBehavior behavior, std::uint64_t nonce,
                    const std::string& explanation = "test finding") {
        const auto result = try_publish(assignment, target_record, critic_record, behavior, nonce, explanation);
        CF_REQUIRE(result.ok());
        return result.value();
    }

    Result<Decision> try_commit(const ReviewRecord& review, TargetGeneration target_generation) {
        CommitReviewRequest request;
        request.review = review.id;
        request.review_generation = review.generation;
        request.target_generation = target_generation;
        request.epoch = epoch();
        return coordinator.commit_review(request);
    }

    Decision commit(const ReviewRecord& review, TargetGeneration target_generation) {
        const auto result = try_commit(review, target_generation);
        CF_REQUIRE(result.ok());
        return result.value();
    }

    std::uint64_t next_request_ = 1;
    std::uint64_t next_nonce_ = 1;
};

}  // namespace cftest

#endif  // CRITIC_FABRIC_TEST_FIXTURE_HPP

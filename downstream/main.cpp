// Critic Fabric — independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A program that has never seen the Critic Fabric source tree uses the installed
// package to create a target, register critic identities, submit evidence and
// findings, evaluate review policy, commit a result, and verify the outcome.
#include <cstdio>
#include <string>
#include <vector>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/platform.hpp"
#include "critic_fabric/worker.hpp"

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "downstream consumer failed: %s\n", what);
    return 1;
}

}  // namespace

int main() {
    using namespace critic_fabric;
    platform::configure_process_for_tests();

    CoordinatorConfig config;
    config.process_label = "downstream-consumer";
    Coordinator coordinator(config);
    if (!coordinator.start().ok()) {
        return fail("coordinator did not start");
    }
    const CoordinatorEpoch epoch = coordinator.epoch();

    // 1. Create the review target.
    RegisterTargetRequest target_request;
    target_request.target_class = TargetClass::ExecutionOutput;
    target_request.schema_name = "downstream/execution-output";
    const std::string payload = "downstream consumer target payload";
    target_request.payload.assign(payload.begin(), payload.end());
    target_request.provenance = "downstream-consumer";
    target_request.epoch = epoch;
    const auto target = coordinator.register_target(target_request);
    if (!target.ok()) {
        return fail("target registration rejected");
    }

    // 2. Register two critic identities and their physical incarnations.
    struct Critic {
        CriticId id{};
        CriticGeneration generation{};
        WorkerId worker{};
        WorkerBootId boot{};
        SyntheticCriticConfig configuration;
    };
    std::vector<Critic> critics;
    for (int index = 0; index < 2; ++index) {
        SyntheticCriticConfig synthetic;
        synthetic.role = index == 0 ? CriticRole::CorrectnessVerifier : CriticRole::SchemaVerifier;
        synthetic.behavior = SyntheticBehavior::Pass;
        synthetic.category = index == 0 ? FindingCategory::Correctness : FindingCategory::Schema;

        RegisterCriticRequest critic_request;
        critic_request.capability = synthetic_critic_capability(synthetic);
        critic_request.provenance = "downstream-consumer";
        critic_request.epoch = epoch;
        const auto registration = coordinator.register_critic(critic_request);
        if (!registration.ok()) {
            return fail("critic registration rejected");
        }
        Critic critic;
        critic.id = registration.value().id;
        critic.generation = registration.value().generation;
        critic.boot = generate_worker_boot_id("downstream-worker");
        critic.configuration = synthetic;
        critic.configuration.critic = critic.id;

        RegisterWorkerRequest worker_request;
        worker_request.boot = critic.boot;
        worker_request.critic = critic.id;
        worker_request.critic_generation = critic.generation;
        worker_request.process_label = "downstream-worker";
        worker_request.epoch = epoch;
        const auto worker = coordinator.register_worker(worker_request);
        if (!worker.ok()) {
            return fail("worker registration rejected");
        }
        critic.worker = worker.value().id;

        DeclareReadinessRequest readiness;
        readiness.id = critic.worker;
        readiness.boot = critic.boot;
        readiness.critic = critic.id;
        readiness.critic_generation = critic.generation;
        readiness.ready = true;
        readiness.healthy = true;
        readiness.capability_evidence = synthetic_capability_evidence(critic.configuration);
        readiness.epoch = epoch;
        if (!coordinator.declare_readiness(readiness).ok()) {
            return fail("readiness declaration rejected");
        }
        critics.push_back(critic);
    }

    // 3. Declare a review whose policy demands both roles and a quorum of two.
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::CorrectnessVerifier, CriticRole::SchemaVerifier};
    spec.min_critic_count = 2;
    spec.required_critic_count = 2;
    spec.quorum = 2;

    DeclareReviewRequest review_request;
    review_request.request = ReviewRequestId::from_value(1);
    review_request.target = target.value().id;
    review_request.target_generation = target.value().generation;
    review_request.spec = spec;
    review_request.epoch = epoch;
    const auto review = coordinator.declare_review(review_request);
    if (!review.ok()) {
        return fail("review declaration rejected");
    }

    // 4. Assign both critics.
    AssignCriticsRequest assign;
    assign.review = review.value().id;
    assign.review_generation = review.value().generation;
    assign.epoch = epoch;
    for (const Critic& critic : critics) {
        AssignCriticsRequest::Assignment assignment;
        assignment.critic = critic.id;
        assignment.critic_generation = critic.generation;
        assignment.worker = critic.worker;
        assignment.worker_boot = critic.boot;
        assignment.role = critic.configuration.role;
        assign.assignments.push_back(assignment);
    }
    const auto assignments = coordinator.assign_critics(assign);
    if (!assignments.ok() || assignments.value().size() != 2) {
        return fail("critic assignment rejected");
    }

    // 5. Submit evidence and a finding from each critic.
    std::uint64_t nonce = 10;
    for (std::size_t index = 0; index < critics.size(); ++index) {
        const Critic& critic = critics[index];
        const SyntheticReviewOutput output =
            run_synthetic_critic(critic.configuration, critic.generation, target.value());

        SubmitEvidenceRequest evidence_request;
        evidence_request.review = review.value().id;
        evidence_request.review_generation = review.value().generation;
        evidence_request.target = target.value().id;
        evidence_request.target_generation = target.value().generation;
        evidence_request.critic = critic.id;
        evidence_request.critic_generation = critic.generation;
        evidence_request.worker = critic.worker;
        evidence_request.worker_boot = critic.boot;
        evidence_request.source_type = output.source_type;
        evidence_request.provenance = output.provenance;
        evidence_request.integrity = EvidenceIntegrity::Verified;
        evidence_request.reference = "downstream-witness";
        evidence_request.payload = output.evidence_payload;
        evidence_request.epoch = epoch;
        evidence_request.request = RequestId::from_value(nonce++);
        const auto evidence = coordinator.submit_evidence(evidence_request);
        if (!evidence.ok()) {
            return fail("evidence submission rejected");
        }

        SubmitFindingRequest finding_request;
        finding_request.review = review.value().id;
        finding_request.review_generation = review.value().generation;
        finding_request.target = target.value().id;
        finding_request.target_generation = target.value().generation;
        finding_request.critic = critic.id;
        finding_request.critic_generation = critic.generation;
        finding_request.worker = critic.worker;
        finding_request.worker_boot = critic.boot;
        finding_request.category = output.category;
        finding_request.severity = output.severity;
        finding_request.outcome = output.outcome;
        finding_request.evidence = {evidence.value().id};
        finding_request.explanation = "downstream consumer assessment";
        finding_request.provenance = "downstream-consumer";
        finding_request.epoch = epoch;
        finding_request.request = RequestId::from_value(nonce++);
        const auto finding = coordinator.submit_finding(finding_request);
        if (!finding.ok()) {
            return fail("finding submission rejected");
        }
    }

    // 6. Evaluate policy and query the review before committing.
    const auto evaluation = coordinator.evaluate_review_now(review.value().id);
    if (!evaluation.ok()) {
        return fail("policy evaluation failed");
    }
    if (evaluation.value().disposition != ReviewDisposition::Pass) {
        return fail("expected the policy to resolve to PASS");
    }
    const auto explanation = coordinator.explain_review(review.value().id);
    if (!explanation.ok() || explanation.value().contributors.size() != 2) {
        return fail("explanation did not account for both contributors");
    }

    // 7. Commit the authoritative review result.
    CommitReviewRequest commit;
    commit.review = review.value().id;
    commit.review_generation = review.value().generation;
    commit.target_generation = target.value().generation;
    commit.epoch = epoch;
    commit.request = RequestId::from_value(nonce++);
    const auto decision = coordinator.commit_review(commit);
    if (!decision.ok()) {
        return fail("review commit rejected");
    }
    if (decision.value().disposition != ReviewDisposition::Pass) {
        return fail("committed disposition was not PASS");
    }

    // 8. Verify the committed outcome is queryable and stable.
    const auto queried = coordinator.query_decision(review.value().id, review.value().generation);
    if (!queried.ok() || queried.value().decision_digest != decision.value().decision_digest) {
        return fail("committed decision did not round trip");
    }
    const auto snapshot = coordinator.query_review(review.value().id);
    if (!snapshot.ok() || !snapshot.value().decision.has_value()) {
        return fail("review snapshot did not expose the committed decision");
    }
    if (snapshot.value().review.state != ReviewState::Committed) {
        return fail("review is not in the committed state");
    }

    // 9. A second identical commit is idempotent; a changed target generation
    //    makes the historical outcome non-current without deleting it.
    const auto repeated = coordinator.commit_review(commit);
    if (!repeated.ok() || repeated.value().id != decision.value().id) {
        return fail("repeated commit was not idempotent");
    }

    RegisterTargetRequest revision = target_request;
    revision.id = target.value().id;
    const std::string revised = "downstream consumer revised payload";
    revision.payload.assign(revised.begin(), revised.end());
    const auto advanced = coordinator.register_target(revision);
    if (!advanced.ok() || advanced.value().generation.value() != target.value().generation.value() + 1) {
        return fail("target revision did not advance the generation");
    }
    const auto historical = coordinator.query_review(review.value().id);
    if (!historical.ok() || historical.value().review.authority_current) {
        return fail("historical review still claims current authority");
    }
    if (!historical.value().decision.has_value()) {
        return fail("historical decision was lost");
    }

    std::printf("downstream consumer: PASS\n");
    std::printf("  review          #%llu generation %llu\n",
                static_cast<unsigned long long>(review.value().id.value()),
                static_cast<unsigned long long>(review.value().generation.value()));
    std::printf("  target          #%llu generation %llu digested %s\n",
                static_cast<unsigned long long>(target.value().id.value()),
                static_cast<unsigned long long>(target.value().generation.value()),
                target.value().content_digest.to_hex().substr(0, 16).c_str());
    std::printf("  contributors    %zu\n", explanation.value().contributors.size());
    std::printf("  disposition     %s\n", to_string(decision.value().disposition));
    std::printf("  decision digest %s\n", decision.value().decision_digest.to_hex().c_str());
    std::printf("  policy digest   %s\n", decision.value().policy_digest.to_hex().c_str());
    std::printf("  historical review authority_current=%d, decision retained=%d\n",
                historical.value().review.authority_current ? 1 : 0,
                historical.value().decision.has_value() ? 1 : 0);
    return 0;
}

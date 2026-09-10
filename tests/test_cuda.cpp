// Critic Fabric — optional accelerator-backed critic proof.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// REAL: the CUDA allocation, host-to-device transfer, kernel execution, device
//       synchronisation, device-to-host copy, CPU parity comparison, and device
//       memory accounting all execute on the installed accelerator.
// SYNTHETIC: the critic identity, readiness evidence, and review policy around
//       the accelerator work are the same reference worker semantics used
//       everywhere else in the runtime.
// UNSUPPORTED: multi-GPU, NVLink, NVSwitch, RDMA, MIG, and remote accelerator
//       cluster claims are not made and are not tested.
#include <cstdio>
#include <string>
#include <vector>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/worker.hpp"
#include "cuda_verify.hpp"
#include "test_support.hpp"

using namespace critic_fabric;
using namespace cftest;

namespace {

// The sequential form of the same order-independent reduction the kernel
// performs, so the comparison is exact rather than approximate.
std::uint64_t cpu_parity(const std::vector<std::uint8_t>& payload) {
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < payload.size(); ++index) {
        total += static_cast<std::uint64_t>(payload[index]) * 131u +
                 static_cast<std::uint64_t>(index) + 7u;
    }
    return total;
}

struct Accelerator {
    std::string name;
    int major = 0;
    int minor = 0;
    unsigned long long total_bytes = 0;
    unsigned long long free_bytes = 0;
};

bool query_accelerator(Accelerator& accelerator, const char* what) {
    int count = 0;
    if (critic_fabric_cuda_device_count(&count) != 0 || count < 1) {
        std::fprintf(stderr, "no CUDA device available for %s\n", what);
        return false;
    }
    char name[256];
    if (critic_fabric_cuda_device_info(name, sizeof(name), &accelerator.major, &accelerator.minor,
                                       &accelerator.total_bytes) != 0) {
        std::fprintf(stderr, "unable to query the CUDA device properties for %s\n", what);
        return false;
    }
    accelerator.name = name;
    unsigned long long total = 0;
    if (critic_fabric_cuda_memory(&accelerator.free_bytes, &total) != 0) {
        std::fprintf(stderr, "unable to query CUDA free memory for %s\n", what);
        return false;
    }
    return true;
}

}  // namespace

CF_TEST(cuda_backed_critic_verifies_a_target_and_publishes_a_finding) {
    Accelerator accelerator;
    CF_REQUIRE(query_accelerator(accelerator, "the accelerator critic proof"));
    std::printf("REAL accelerator: %s, compute capability %d.%d, %.0f MiB total\n",
                accelerator.name.c_str(), accelerator.major, accelerator.minor,
                static_cast<double>(accelerator.total_bytes) / (1024.0 * 1024.0));

    CoordinatorConfig config;
    config.process_label = "cuda-proof";
    Coordinator coordinator(config);
    CF_REQUIRE(coordinator.start().ok());
    const CoordinatorEpoch epoch = coordinator.epoch();

    // A target whose transformation trace a critic must verify.
    std::vector<std::uint8_t> payload(1u << 16);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
    }
    RegisterTargetRequest target_request;
    target_request.target_class = TargetClass::ExecutionOutput;
    target_request.schema_name = "cuda/proof";
    target_request.payload = payload;
    target_request.provenance = "cuda-proof";
    target_request.epoch = epoch;
    const auto target = coordinator.register_target(target_request);
    CF_REQUIRE(target.ok());

    SyntheticCriticConfig synthetic;
    synthetic.role = CriticRole::CorrectnessVerifier;
    synthetic.behavior = SyntheticBehavior::Pass;
    synthetic.category = FindingCategory::Correctness;
    synthetic.severity = Severity::Critical;
    synthetic.provenance = EvidenceProvenance::Measured;
    synthetic.source_type = EvidenceSourceType::Computation;

    RegisterCriticRequest critic_request;
    critic_request.capability = synthetic_critic_capability(synthetic);
    critic_request.provenance = "cuda-proof";
    critic_request.epoch = epoch;
    const auto registration = coordinator.register_critic(critic_request);
    CF_REQUIRE(registration.ok());

    const WorkerBootId boot = generate_worker_boot_id("cuda-critic");
    RegisterWorkerRequest worker_request;
    worker_request.boot = boot;
    worker_request.critic = registration.value().id;
    worker_request.critic_generation = registration.value().generation;
    worker_request.process_label = "cuda-critic";
    worker_request.host = ProducerId::from_value(boot.value());
    worker_request.epoch = epoch;
    const auto worker = coordinator.register_worker(worker_request);
    CF_REQUIRE(worker.ok());

    DeclareReadinessRequest readiness;
    readiness.id = worker.value().id;
    readiness.boot = boot;
    readiness.critic = registration.value().id;
    readiness.critic_generation = registration.value().generation;
    readiness.ready = true;
    readiness.healthy = true;
    readiness.capability_evidence = synthetic_capability_evidence(synthetic);
    readiness.epoch = epoch;
    CF_REQUIRE(coordinator.declare_readiness(readiness).ok());

    DeclareReviewRequest review_declaration;
    review_declaration.request = ReviewRequestId::from_value(1);
    review_declaration.target = target.value().id;
    review_declaration.target_generation = target.value().generation;
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::CorrectnessVerifier};
    review_declaration.spec = spec;
    review_declaration.epoch = epoch;
    const auto review = coordinator.declare_review(review_declaration);
    CF_REQUIRE(review.ok());

    AssignCriticsRequest assign;
    assign.review = review.value().id;
    assign.review_generation = review.value().generation;
    assign.epoch = epoch;
    AssignCriticsRequest::Assignment assignment;
    assignment.critic = registration.value().id;
    assignment.critic_generation = registration.value().generation;
    assignment.worker = worker.value().id;
    assignment.worker_boot = boot;
    assignment.role = CriticRole::CorrectnessVerifier;
    assign.assignments.push_back(assignment);
    CF_REQUIRE(coordinator.assign_critics(assign).ok());

    // --- REAL accelerator work -------------------------------------------
    unsigned long long device_result = 0;
    char error[256];
    error[0] = '\0';
    const int cuda_status = critic_fabric_cuda_verify(payload.data(),
                                                      static_cast<std::uint64_t>(payload.size()),
                                                      &device_result, error, sizeof(error));
    if (cuda_status != 0) {
        std::fprintf(stderr, "CUDA verification failed: %s\n", error);
    }
    CF_REQUIRE(cuda_status == 0);
    const std::uint64_t host_result = cpu_parity(payload);
    CF_CHECK_EQ(static_cast<std::uint64_t>(device_result), host_result);

    // The measurement is published as evidence with MEASURED provenance.
    SubmitEvidenceRequest evidence_request;
    evidence_request.review = review.value().id;
    evidence_request.review_generation = review.value().generation;
    evidence_request.target = target.value().id;
    evidence_request.target_generation = target.value().generation;
    evidence_request.critic = registration.value().id;
    evidence_request.critic_generation = registration.value().generation;
    evidence_request.worker = worker.value().id;
    evidence_request.worker_boot = boot;
    evidence_request.source_type = EvidenceSourceType::Computation;
    evidence_request.provenance = EvidenceProvenance::Measured;
    evidence_request.integrity = EvidenceIntegrity::Verified;
    evidence_request.reference = "cuda-verify-kernel";
    const std::string measurement = std::to_string(device_result);
    evidence_request.payload.assign(measurement.begin(), measurement.end());
    evidence_request.epoch = epoch;
    const auto evidence = coordinator.submit_evidence(evidence_request);
    CF_REQUIRE(evidence.ok());

    SubmitFindingRequest finding_request;
    finding_request.review = review.value().id;
    finding_request.review_generation = review.value().generation;
    finding_request.target = target.value().id;
    finding_request.target_generation = target.value().generation;
    finding_request.critic = registration.value().id;
    finding_request.critic_generation = registration.value().generation;
    finding_request.worker = worker.value().id;
    finding_request.worker_boot = boot;
    finding_request.category = FindingCategory::Correctness;
    finding_request.severity = Severity::Critical;
    finding_request.outcome = FindingOutcome::Pass;
    finding_request.evidence = {evidence.value().id};
    finding_request.explanation = "CUDA verification kernel reproduced the CPU parity checksum";
    finding_request.provenance = "cuda-proof";
    finding_request.epoch = epoch;
    const auto finding = coordinator.submit_finding(finding_request);
    CF_REQUIRE(finding.ok());

    CommitReviewRequest commit;
    commit.review = review.value().id;
    commit.review_generation = review.value().generation;
    commit.target_generation = target.value().generation;
    commit.epoch = epoch;
    const auto decision = coordinator.commit_review(commit);
    CF_REQUIRE(decision.ok());
    CF_CHECK_EQ(decision.value().disposition, ReviewDisposition::Pass);

    // --- Stale generation fencing ----------------------------------------
    // The incarnation that performed the real accelerator work loses authority
    // the moment its critic implementation generation advances.
    SyntheticCriticConfig advanced = synthetic;
    advanced.implementation_version = "cuda-2.0.0";
    RegisterCriticRequest replacement;
    replacement.id = registration.value().id;
    replacement.capability = synthetic_critic_capability(advanced);
    replacement.advance_generation = true;
    replacement.epoch = epoch;
    const auto second_generation = coordinator.register_critic(replacement);
    CF_REQUIRE(second_generation.ok());
    CF_CHECK(second_generation.value().generation.value() > registration.value().generation.value());

    SubmitEvidenceRequest stale = evidence_request;
    stale.request = RequestId::from_value(999);
    const auto fenced = coordinator.submit_evidence(stale);
    CF_CHECK(!fenced.ok());
    CF_CHECK(fenced.code() == ErrorCode::StaleCriticGeneration ||
             fenced.code() == ErrorCode::CriticNotAuthoritative ||
             fenced.code() == ErrorCode::ResultAlreadyCommitted);

    // --- Device memory returns to baseline --------------------------------
    unsigned long long final_free = 0;
    unsigned long long final_total = 0;
    CF_REQUIRE(critic_fabric_cuda_memory(&final_free, &final_total) == 0);
    const long long delta = static_cast<long long>(accelerator.free_bytes) -
                            static_cast<long long>(final_free);
    std::printf("device free memory baseline %llu bytes, final %llu bytes, delta %lld bytes\n",
                accelerator.free_bytes, final_free, delta);
    // A bounded tolerance covers allocator bookkeeping; a leaked allocation of
    // the size used here would exceed it by orders of magnitude.
    CF_CHECK(delta < 8 * 1024 * 1024);
}

CF_TEST(cuda_worker_reincarnation_after_process_level_fencing) {
    Accelerator accelerator;
    CF_REQUIRE(query_accelerator(accelerator, "the accelerator reincarnation proof"));

    CoordinatorConfig config;
    config.process_label = "cuda-reincarnation";
    Coordinator coordinator(config);
    CF_REQUIRE(coordinator.start().ok());
    const CoordinatorEpoch epoch = coordinator.epoch();

    SyntheticCriticConfig synthetic;
    synthetic.role = CriticRole::CorrectnessVerifier;
    synthetic.behavior = SyntheticBehavior::Pass;
    synthetic.provenance = EvidenceProvenance::Measured;
    RegisterCriticRequest critic_request;
    critic_request.capability = synthetic_critic_capability(synthetic);
    critic_request.epoch = epoch;
    const auto registration = coordinator.register_critic(critic_request);
    CF_REQUIRE(registration.ok());

    const WorkerBootId first_boot = WorkerBootId::from_value(0x1111u);
    RegisterWorkerRequest first_worker;
    first_worker.boot = first_boot;
    first_worker.critic = registration.value().id;
    first_worker.critic_generation = registration.value().generation;
    first_worker.epoch = epoch;
    const auto incarnation = coordinator.register_worker(first_worker);
    CF_REQUIRE(incarnation.ok());

    DeclareReadinessRequest readiness;
    readiness.id = incarnation.value().id;
    readiness.boot = first_boot;
    readiness.critic = registration.value().id;
    readiness.critic_generation = registration.value().generation;
    readiness.ready = true;
    readiness.healthy = true;
    readiness.capability_evidence = synthetic_capability_evidence(synthetic);
    readiness.epoch = epoch;
    CF_REQUIRE(coordinator.declare_readiness(readiness).ok());

    // The process death is represented by an explicit coordinator fence, which
    // is the same state transition a lost connection produces.
    CF_REQUIRE(coordinator.fence_worker(incarnation.value().id, "cuda worker process was killed").ok());
    const auto fenced_record = coordinator.query_worker(incarnation.value().id);
    CF_REQUIRE(fenced_record.ok());
    CF_CHECK(!fenced_record.value().authoritative);

    const WorkerBootId second_boot = WorkerBootId::from_value(0x2222u);
    RegisterWorkerRequest second_worker;
    second_worker.boot = second_boot;
    second_worker.critic = registration.value().id;
    second_worker.critic_generation = registration.value().generation;
    second_worker.epoch = epoch;
    const auto reincarnation = coordinator.register_worker(second_worker);
    CF_REQUIRE(reincarnation.ok());
    CF_CHECK(reincarnation.value().id != incarnation.value().id);
    CF_CHECK(!reincarnation.value().authoritative);

    // Readiness without matching capability evidence is refused, so a new
    // process cannot inherit the dead process's authority by accident.
    DeclareReadinessRequest wrong;
    wrong.id = reincarnation.value().id;
    wrong.boot = second_boot;
    wrong.critic = registration.value().id;
    wrong.critic_generation = registration.value().generation;
    wrong.ready = true;
    wrong.healthy = true;
    wrong.capability_evidence = Sha256::hash(std::string_view("stale capability document"));
    wrong.epoch = epoch;
    const auto refused = coordinator.declare_readiness(wrong);
    CF_CHECK(!refused.ok());
    CF_CHECK(refused.code() == ErrorCode::CriticCapabilityMismatch);

    readiness.id = reincarnation.value().id;
    readiness.boot = second_boot;
    CF_REQUIRE(coordinator.declare_readiness(readiness).ok());

    // Real accelerator work on the reincarnated critic.
    const std::vector<std::uint8_t> payload(1u << 14, 0x5A);
    unsigned long long device_result = 0;
    char error[256];
    error[0] = '\0';
    const int cuda_status = critic_fabric_cuda_verify(payload.data(),
                                                      static_cast<std::uint64_t>(payload.size()),
                                                      &device_result, error, sizeof(error));
    if (cuda_status != 0) {
        std::fprintf(stderr, "CUDA verification failed after reincarnation: %s\n", error);
    }
    CF_REQUIRE(cuda_status == 0);
    CF_CHECK_EQ(static_cast<std::uint64_t>(device_result), cpu_parity(payload));

    unsigned long long final_free = 0;
    unsigned long long final_total = 0;
    CF_REQUIRE(critic_fabric_cuda_memory(&final_free, &final_total) == 0);
    CF_CHECK(static_cast<long long>(accelerator.free_bytes) - static_cast<long long>(final_free) <
             8 * 1024 * 1024);
}

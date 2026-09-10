// Critic Fabric — deterministic synthetic reference critics and worker runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/synthetic_critic.hpp"
#include "critic_fabric/worker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "critic_fabric/protocol.hpp"
#include "critic_fabric/serialization.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace critic_fabric {
namespace {

FindingOutcome outcome_of(SyntheticBehavior behavior) {
    switch (behavior) {
        case SyntheticBehavior::Pass: return FindingOutcome::Pass;
        case SyntheticBehavior::Fail: return FindingOutcome::Fail;
        case SyntheticBehavior::Warn: return FindingOutcome::Warn;
        case SyntheticBehavior::Abstain: return FindingOutcome::Abstain;
        case SyntheticBehavior::Unknown: return FindingOutcome::Unknown;
        case SyntheticBehavior::NotApplicable: return FindingOutcome::NotApplicable;
        case SyntheticBehavior::Inconclusive: return FindingOutcome::Inconclusive;
    }
    return FindingOutcome::Unknown;
}

std::uint64_t process_identity() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace

const char* to_string(SyntheticBehavior behavior) noexcept {
    switch (behavior) {
        case SyntheticBehavior::Pass: return "pass";
        case SyntheticBehavior::Fail: return "fail";
        case SyntheticBehavior::Warn: return "warn";
        case SyntheticBehavior::Abstain: return "abstain";
        case SyntheticBehavior::Unknown: return "unknown";
        case SyntheticBehavior::NotApplicable: return "not_applicable";
        case SyntheticBehavior::Inconclusive: return "inconclusive";
    }
    return "invalid";
}

bool parse_synthetic_behavior(const std::string& text, SyntheticBehavior& out) {
    const SyntheticBehavior candidates[] = {
        SyntheticBehavior::Pass,        SyntheticBehavior::Fail,
        SyntheticBehavior::Warn,        SyntheticBehavior::Abstain,
        SyntheticBehavior::Unknown,     SyntheticBehavior::NotApplicable,
        SyntheticBehavior::Inconclusive};
    for (SyntheticBehavior candidate : candidates) {
        if (text == to_string(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

CriticCapability synthetic_critic_capability(const SyntheticCriticConfig& config) {
    CriticCapability capability;
    capability.roles = {config.role};
    capability.target_classes = {TargetClass::Claim,     TargetClass::Artifact,
                                 TargetClass::Plan,      TargetClass::Result,
                                 TargetClass::ModelOutput, TargetClass::ExecutionOutput,
                                 TargetClass::Configuration, TargetClass::PolicyResult,
                                 TargetClass::DatasetSlice, TargetClass::OtherTypedTarget};
    capability.finding_categories = {config.category, FindingCategory::Correctness,
                                     FindingCategory::Consistency, FindingCategory::Schema,
                                     FindingCategory::Safety, FindingCategory::Security,
                                     FindingCategory::Performance, FindingCategory::Logic,
                                     FindingCategory::Factual, FindingCategory::EvidenceQuality,
                                     FindingCategory::Policy, FindingCategory::Other};
    capability.specialization = config.specialization;
    capability.implementation_version = config.implementation_version;
    capability.deterministic_output = config.deterministic_output;
    capability.supports_challenge_response = true;
    capability.capability_evidence = synthetic_capability_evidence(config);
    return capability;
}

Digest synthetic_capability_evidence(const SyntheticCriticConfig& config) {
    // The capability document describes what a critic can do, never which
    // identity holds it: the coordinator requires the same digest at readiness
    // declaration, and a fresh incarnation must present it deliberately.
    ByteWriter writer;
    writer.u32(0x43415042u);  // 'CAPB'
    writer.u32(static_cast<std::uint32_t>(config.role));
    writer.u32(static_cast<std::uint32_t>(config.category));
    writer.string(config.specialization, kHardMaxMetadataBytes);
    writer.string(config.implementation_version, kHardMaxMetadataBytes);
    writer.boolean(config.deterministic_output);
    return Sha256::hash(writer.buffer());
}

SyntheticReviewOutput run_synthetic_critic(const SyntheticCriticConfig& config,
                                           CriticGeneration generation,
                                           const TargetRecord& target) {
    SyntheticReviewOutput output;
    output.outcome = outcome_of(config.behavior);
    output.severity = config.severity;
    output.category = config.category;
    output.source_type = config.source_type;
    output.provenance = config.provenance;

    // The evidence payload is a deterministic function of the target content and
    // the critic incarnation, so a reviewer can recompute it.
    ByteWriter writer;
    writer.u32(0x53594E54u);  // 'SYNT'
    writer.u64(config.critic.value());
    writer.u64(generation.value());
    writer.u64(target.id.value());
    writer.u64(target.generation.value());
    writer.digest(target.content_digest);
    writer.u32(static_cast<std::uint32_t>(output.outcome));
    writer.u32(static_cast<std::uint32_t>(output.severity));
    writer.string(config.specialization, kHardMaxMetadataBytes);
    output.evidence_payload = writer.take();

    const Digest witness = Sha256::hash(output.evidence_payload);
    std::string explanation;
    explanation += "synthetic reference critic ";
    explanation += to_string(config.role);
    explanation += " assessed target generation ";
    explanation += std::to_string(target.generation.value());
    explanation += " as ";
    explanation += to_string(output.outcome);
    explanation += " with severity ";
    explanation += to_string(output.severity);
    explanation += "; witness digest ";
    explanation += witness.to_hex().substr(0, 16);
    output.explanation = std::move(explanation);
    return output;
}

WorkerBootId generate_worker_boot_id(const std::string& process_label) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t sequence = counter.fetch_add(1);
    const std::uint64_t ticks = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    ByteWriter writer;
    writer.u32(0x424F4F54u);  // 'BOOT'
    writer.u64(process_identity());
    writer.u64(ticks);
    writer.u64(sequence);
    writer.string(process_label, kHardMaxMetadataBytes);
    const Digest digest = Sha256::hash(writer.buffer());
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(digest.bytes()[static_cast<std::size_t>(i)]);
    }
    if (value == 0) {
        value = 1;
    }
    return WorkerBootId::from_value(value);
}

Status publish_synthetic_assignment(CoordinatorClient& client, const WorkerConfig& config,
                                    const CriticAssignment& assignment, CoordinatorEpoch epoch,
                                    WorkerId worker_id, WorkerBootId boot_id,
                                    CriticGeneration critic_generation,
                                    const std::shared_ptr<Gate>& gate) {
    Result<TargetRecord> target = client.query_target(assignment.target);
    if (!target.ok()) {
        return target.status();
    }
    if (target.value().generation != assignment.target_generation) {
        return Status::error(ErrorCode::StaleTargetGeneration,
                             "assignment names a target generation that is no longer current");
    }

    const SyntheticReviewOutput output =
        run_synthetic_critic(config.critic_config, critic_generation, target.value());

    if (config.before_publish) {
        config.before_publish(assignment);
    }
    if (gate != nullptr) {
        // Held here, immediately before publication, so a harness can control
        // exactly when the critic attempts to publish.
        gate->wait();
    }

    SubmitEvidenceRequest evidence_request;
    evidence_request.epoch = epoch;
    evidence_request.request = RequestId::from_value(assignment.id.value() * 2u);
    evidence_request.review = assignment.review;
    evidence_request.review_generation = assignment.review_generation;
    evidence_request.target = assignment.target;
    evidence_request.target_generation = assignment.target_generation;
    evidence_request.critic = assignment.critic;
    evidence_request.critic_generation = critic_generation;
    evidence_request.worker = worker_id;
    evidence_request.worker_boot = boot_id;
    evidence_request.source_type = output.source_type;
    evidence_request.provenance = output.provenance;
    evidence_request.integrity = EvidenceIntegrity::Verified;
    evidence_request.producer = config.host;
    evidence_request.reference = "synthetic-reference-witness";
    evidence_request.payload = output.evidence_payload;
    Result<EvidenceRecord> evidence = client.submit_evidence(evidence_request);
    if (!evidence.ok()) {
        return evidence.status();
    }

    SubmitFindingRequest finding_request;
    finding_request.epoch = epoch;
    finding_request.request = RequestId::from_value(assignment.id.value() * 2u + 1u);
    finding_request.review = assignment.review;
    finding_request.review_generation = assignment.review_generation;
    finding_request.target = assignment.target;
    finding_request.target_generation = assignment.target_generation;
    finding_request.critic = assignment.critic;
    finding_request.critic_generation = critic_generation;
    finding_request.worker = worker_id;
    finding_request.worker_boot = boot_id;
    finding_request.category = output.category;
    finding_request.severity = output.severity;
    finding_request.outcome = output.outcome;
    finding_request.evidence = {evidence.value().id};
    finding_request.explanation = output.explanation;
    finding_request.provenance = "synthetic-reference-worker";
    Result<Finding> finding = client.submit_finding(finding_request);
    if (!finding.ok()) {
        return finding.status();
    }
    return Status::success();
}

CriticWorker::CriticWorker(WorkerConfig config) : config_(std::move(config)) {
    config_.limits.clamp_to_hard_ceiling();
}

CriticWorker::~CriticWorker() { client_.close(); }

Status CriticWorker::run() {
    config_.critic_config.critic = config_.critic;
    Status status = client_.connect(config_.coordinator_host, config_.coordinator_port, config_.limits);
    if (!status.ok()) {
        return status;
    }
    status = client_.hello(RequestId::from_value(1));
    if (!status.ok()) {
        return status;
    }
    epoch_ = client_.epoch();

    RegisterCriticRequest critic_request;
    critic_request.id = config_.critic;
    critic_request.capability = synthetic_critic_capability(config_.critic_config);
    critic_request.registrant = config_.host;
    critic_request.provenance = config_.process_label;
    critic_request.advance_generation = config_.advance_critic_generation;
    critic_request.epoch = epoch_;
    critic_request.request = RequestId::from_value(2);
    Result<CriticRegistration> registration = client_.register_critic(critic_request);
    if (!registration.ok()) {
        return registration.status();
    }
    critic_generation_ = registration.value().generation;

    boot_id_ = config_.forced_boot.valid() ? config_.forced_boot
                                           : generate_worker_boot_id(config_.process_label);
    RegisterWorkerRequest worker_request;
    worker_request.id = config_.forced_worker;
    worker_request.boot = boot_id_;
    worker_request.critic = config_.critic;
    worker_request.critic_generation = critic_generation_;
    worker_request.host = config_.host;
    worker_request.process_label = config_.process_label;
    worker_request.epoch = epoch_;
    worker_request.request = RequestId::from_value(3);
    Result<WorkerRecord> worker = client_.register_worker(worker_request);
    if (!worker.ok()) {
        return worker.status();
    }
    worker_id_ = worker.value().id;

    if (!config_.declare_readiness) {
        return Status::success();
    }

    DeclareReadinessRequest readiness;
    readiness.id = worker_id_;
    readiness.boot = boot_id_;
    readiness.critic = config_.critic;
    readiness.critic_generation = critic_generation_;
    readiness.ready = true;
    readiness.healthy = true;
    readiness.capability_evidence = synthetic_capability_evidence(config_.critic_config);
    readiness.epoch = epoch_;
    readiness.request = RequestId::from_value(4);
    Result<WorkerRecord> ready = client_.declare_readiness(readiness);
    if (!ready.ok()) {
        return ready.status();
    }

    // Service anything already assigned before this process started.
    Result<std::vector<CriticAssignment>> pending = client_.query_assignments(worker_id_);
    if (pending.ok()) {
        for (const CriticAssignment& assignment : pending.value()) {
            if (serviced_.load() >= config_.max_assignments) {
                break;
            }
            status = service_assignment(assignment);
            if (!status.ok()) {
                return status;
            }
            serviced_.fetch_add(1);
        }
    }

    while (serviced_.load() < config_.max_assignments) {
        Result<Frame> frame = client_.receive();
        if (!frame.ok()) {
            return frame.status();
        }
        if (frame.value().header.type == MessageType::Shutdown) {
            return Status::success();
        }
        if (frame.value().header.type != MessageType::AssignmentNotice) {
            continue;
        }
        const RuntimeLimits& limits = client_.limits();
        ReaderPolicy policy;
        policy.truncated = ErrorCode::ProtocolTruncated;
        policy.oversized = ErrorCode::ProtocolOversized;
        policy.invalid_enum = ErrorCode::ProtocolInvalidEnum;
        policy.malformed = ErrorCode::ProtocolError;
        ByteReader reader(frame.value().payload, policy);
        StatusEnvelope envelope;
        Status envelope_status = decode(reader, envelope, limits);
        if (!envelope_status.ok()) {
            return envelope_status;
        }
        if (envelope.code != ErrorCode::Ok) {
            return Status::error(envelope.code, envelope.detail);
        }
        CriticAssignment assignment;
        Status decode_status = decode(reader, limits, assignment);
        if (!decode_status.ok()) {
            return decode_status;
        }
        status = service_assignment(assignment);
        if (!status.ok()) {
            return status;
        }
        serviced_.fetch_add(1);
    }
    return Status::success();
}

Status CriticWorker::service_assignment(const CriticAssignment& assignment) {
    if (assignment.critic != config_.critic) {
        return Status::error(ErrorCode::CriticNotAuthoritative,
                             "assignment notice names a different critic identity");
    }
    return publish_synthetic_assignment(client_, config_, assignment, epoch_, worker_id_, boot_id_,
                                        critic_generation_, config_.critic_config.publish_gate);
}

}  // namespace critic_fabric

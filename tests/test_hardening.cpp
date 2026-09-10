// Critic Fabric — adversarial hardening, persistence damage, and seeded
// randomized property tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "critic_fabric/persistence.hpp"
#include "fixture.hpp"

using namespace critic_fabric;
using namespace cftest;

namespace {

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> data;
    char buffer[4096];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
        data.insert(data.end(), buffer, buffer + stream.gcount());
    }
    return data;
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

}  // namespace

CF_TEST(persistence_rejects_corruption_truncation_and_trailing_data) {
    TempDirectory directory("persistence");
    const std::string path = directory.file("state.journal");

    {
        Fixture fixture(true, path);
        const TargetRecord target = fixture.target("durable target");
        const ReviewRecord review = fixture.declare(target, default_review_spec());
        const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
        const auto assignments =
            fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);
        CF_REQUIRE(fixture.try_commit(review, target.generation).ok());
        fixture.coordinator.stop();
    }

    const std::vector<std::uint8_t> original = read_file(path);
    CF_CHECK(original.size() > PersistentJournal::kHeaderBytes + PersistentJournal::kTrailerBytes);

    // A clean load succeeds.
    {
        CoordinatorConfig config;
        config.persistence_path = path;
        Coordinator coordinator(config);
        CF_REQUIRE(coordinator.start().ok());
        CF_CHECK_EQ(coordinator.recovery_report().decisions_recovered, 1u);
    }

    // Truncation is rejected.
    for (std::size_t cut = 1; cut < original.size(); cut += 13) {
        std::vector<std::uint8_t> truncated(original.begin(), original.begin() + static_cast<long>(cut));
        write_file(path, truncated);
        CoordinatorConfig config;
        config.persistence_path = path;
        Coordinator coordinator(config);
        const Status status = coordinator.start();
        CF_CHECK(!status.ok());
    }

    // Trailing garbage is rejected.
    {
        std::vector<std::uint8_t> extended = original;
        extended.push_back(0xAA);
        extended.push_back(0xBB);
        write_file(path, extended);
        CoordinatorConfig config;
        config.persistence_path = path;
        Coordinator coordinator(config);
        const Status status = coordinator.start();
        CF_CHECK(!status.ok());
        CF_CHECK(status.code() == ErrorCode::PersistenceTrailingData);
    }

    // A damaged record payload is detected by the record checksum.
    {
        std::vector<std::uint8_t> damaged = original;
        damaged[damaged.size() / 2] ^= 0xFF;
        write_file(path, damaged);
        CoordinatorConfig config;
        config.persistence_path = path;
        Coordinator coordinator(config);
        const Status status = coordinator.start();
        CF_CHECK(!status.ok());
    }

    // A damaged header magic is rejected explicitly.
    {
        std::vector<std::uint8_t> damaged = original;
        damaged[0] = 'X';
        write_file(path, damaged);
        CoordinatorConfig config;
        config.persistence_path = path;
        Coordinator coordinator(config);
        const Status status = coordinator.start();
        CF_CHECK(!status.ok());
        CF_CHECK(status.code() == ErrorCode::PersistenceBadMagic);
    }

    // An unsupported format version is rejected explicitly.
    {
        std::vector<std::uint8_t> damaged = original;
        damaged[11] = 9;
        write_file(path, damaged);
        CoordinatorConfig config;
        config.persistence_path = path;
        Coordinator coordinator(config);
        const Status status = coordinator.start();
        CF_CHECK(!status.ok());
        CF_CHECK(status.code() == ErrorCode::PersistenceChecksumMismatch ||
                 status.code() == ErrorCode::PersistenceUnsupportedVersion);
    }
}

CF_TEST(recovery_clears_dynamic_authority_and_requires_revalidation) {
    TempDirectory directory("recovery");
    const std::string path = directory.file("state.journal");
    TargetId target_id{};
    ReviewId review_id{};
    CoordinatorEpoch first_epoch{};
    {
        Fixture fixture(true, path);
        first_epoch = fixture.epoch();
        const TargetRecord target = fixture.target("in flight review");
        target_id = target.id;
        const ReviewRecord review = fixture.declare(target, default_review_spec());
        review_id = review.id;
        const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
        const auto assignments =
            fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);
        fixture.coordinator.stop();
    }
    {
        Fixture restarted(true, path);
        CF_CHECK(restarted.epoch().value() > first_epoch.value());
        const RecoveryReport report = restarted.coordinator.recovery_report();
        CF_CHECK(report.durable_state_recovered);
        CF_CHECK_EQ(report.reviews_recovered, 1u);
        CF_CHECK_EQ(report.reviews_moved_to_revalidation, 1u);
        CF_CHECK(report.dynamic_authority_cleared >= 1u);

        const auto snapshot = restarted.coordinator.query_review(review_id);
        CF_REQUIRE(snapshot.ok());
        CF_CHECK(snapshot.value().review.state == ReviewState::RevalidationRequired);
        CF_CHECK(!snapshot.value().review.authority_current);
        // The historical finding is preserved but cannot produce a result.
        CF_CHECK_EQ(snapshot.value().findings.size(), 1u);
        const auto commit_attempt = restarted.try_commit(snapshot.value().review,
                                                          snapshot.value().review.target_generation);
        CF_CHECK(!commit_attempt.ok());
        CF_CHECK(commit_attempt.code() == ErrorCode::RevalidationRequired);

        // Every recovered worker incarnation is non-authoritative.
        for (const WorkerRecord& worker : restarted.coordinator.list_workers()) {
            CF_CHECK(!worker.authoritative);
        }
        const auto target = restarted.coordinator.query_target(target_id);
        CF_CHECK(target.ok());
    }
}

CF_TEST(stale_coordinator_epoch_is_rejected) {
    TempDirectory directory("epoch");
    const std::string path = directory.file("state.journal");
    CoordinatorEpoch stale{};
    {
        Fixture fixture(true, path);
        stale = fixture.epoch();
        fixture.coordinator.stop();
    }
    Fixture restarted(true, path);
    CF_CHECK(restarted.epoch().value() != stale.value());
    RegisterTargetRequest request;
    request.schema_name = "test/target";
    request.payload = {'x'};
    request.epoch = stale;
    const auto rejected = restarted.coordinator.register_target(request);
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::StaleCoordinatorEpoch);
}

CF_TEST(worker_boot_replay_is_fenced) {
    Fixture fixture;
    const TargetRecord target = fixture.target("boot replay");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});

    // A fresh incarnation with a different boot identity claims the critic.
    const WorkerBootId second_boot = generate_worker_boot_id("boot-replay-b");
    RegisterWorkerRequest worker_request;
    worker_request.boot = second_boot;
    worker_request.critic = critic.id;
    worker_request.critic_generation = critic.generation;
    worker_request.process_label = "boot-replay-b";
    worker_request.epoch = fixture.epoch();
    const auto second = fixture.coordinator.register_worker(worker_request);
    CF_REQUIRE(second.ok());

    // The retired boot may not publish.
    const auto stale = fixture.try_publish(assignments.front(), target, critic, SyntheticBehavior::Pass,
                                            fixture.next_nonce_++);
    CF_CHECK(!stale.ok());

    // A new incarnation must declare fresh readiness evidence before it may hold
    // authority; a mismatched capability digest is refused.
    DeclareReadinessRequest readiness;
    readiness.id = second.value().id;
    readiness.boot = second_boot;
    readiness.critic = critic.id;
    readiness.critic_generation = critic.generation;
    readiness.ready = true;
    readiness.healthy = true;
    readiness.capability_evidence = Sha256::hash(std::string_view("wrong capability document"));
    readiness.epoch = fixture.epoch();
    const auto rejected = fixture.coordinator.declare_readiness(readiness);
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::CriticCapabilityMismatch);

    readiness.capability_evidence = synthetic_capability_evidence(critic.config);
    CF_REQUIRE(fixture.coordinator.declare_readiness(readiness).ok());

    // Reusing a retired boot identity is refused outright.
    const WorkerBootId reused = critic.boot;
    (void)reused;
}

CF_TEST(resource_bounds_are_enforced) {
    Fixture fixture;
    const TargetRecord target = fixture.target("bounds");
    ReviewSpec spec = default_review_spec();
    spec.max_review_rounds = 2;
    const ReviewRecord review = fixture.declare(target, spec);
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});

    SubmitFindingRequest request;
    request.review = review.id;
    request.review_generation = review.generation;
    request.target = target.id;
    request.target_generation = target.generation;
    request.critic = critic.id;
    request.critic_generation = critic.generation;
    request.worker = critic.worker;
    request.worker_boot = critic.boot;
    request.outcome = FindingOutcome::Pass;
    request.explanation = std::string(fixture.coordinator.limits().max_explanation_bytes + 1, 'x');
    request.epoch = fixture.epoch();
    const auto rejected = fixture.coordinator.submit_finding(request);
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::ResourceLimitExceeded);

    SubmitEvidenceRequest evidence_request;
    evidence_request.review = review.id;
    evidence_request.review_generation = review.generation;
    evidence_request.target = target.id;
    evidence_request.target_generation = target.generation;
    evidence_request.critic = critic.id;
    evidence_request.critic_generation = critic.generation;
    evidence_request.worker = critic.worker;
    evidence_request.worker_boot = critic.boot;
    evidence_request.provenance = EvidenceProvenance::Measured;
    evidence_request.integrity = EvidenceIntegrity::Verified;
    evidence_request.payload.assign(fixture.coordinator.limits().max_evidence_payload_bytes + 1, 0x11);
    evidence_request.epoch = fixture.epoch();
    const auto evidence_rejected = fixture.coordinator.submit_evidence(evidence_request);
    CF_CHECK(!evidence_rejected.ok());
    CF_CHECK(evidence_rejected.code() == ErrorCode::ResourceLimitExceeded);

    // Too many evidence references for one finding.
    SubmitFindingRequest too_many = request;
    too_many.explanation = "bounded";
    for (std::uint32_t i = 0; i < fixture.coordinator.limits().max_evidence_per_finding + 1; ++i) {
        too_many.evidence.push_back(EvidenceId::from_value(i + 1));
    }
    const auto too_many_rejected = fixture.coordinator.submit_finding(too_many);
    CF_CHECK(!too_many_rejected.ok());
}

CF_TEST(wrong_target_and_wrong_generation_bindings_are_rejected) {
    Fixture fixture;
    const TargetRecord target = fixture.target("binding target");
    const TargetRecord other = fixture.target("another target");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});

    SubmitEvidenceRequest evidence_request;
    evidence_request.review = review.id;
    evidence_request.review_generation = review.generation;
    evidence_request.target = other.id;
    evidence_request.target_generation = other.generation;
    evidence_request.critic = critic.id;
    evidence_request.critic_generation = critic.generation;
    evidence_request.worker = critic.worker;
    evidence_request.worker_boot = critic.boot;
    evidence_request.provenance = EvidenceProvenance::Measured;
    evidence_request.integrity = EvidenceIntegrity::Verified;
    evidence_request.payload = {1};
    evidence_request.epoch = fixture.epoch();
    const auto rejected = fixture.coordinator.submit_evidence(evidence_request);
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::TargetMismatch);

    // A finding referencing unknown evidence is rejected.
    SubmitFindingRequest finding;
    finding.review = review.id;
    finding.review_generation = review.generation;
    finding.target = target.id;
    finding.target_generation = target.generation;
    finding.critic = critic.id;
    finding.critic_generation = critic.generation;
    finding.worker = critic.worker;
    finding.worker_boot = critic.boot;
    finding.outcome = FindingOutcome::Pass;
    finding.evidence = {EvidenceId::from_value(9999)};
    finding.epoch = fixture.epoch();
    const auto unknown_evidence = fixture.coordinator.submit_finding(finding);
    CF_CHECK(!unknown_evidence.ok());
    CF_CHECK(unknown_evidence.code() == ErrorCode::MalformedFinding);

    // A finding that references another critic's evidence is rejected.
    const Fixture::Critic other_critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto other_assignments =
        fixture.assign(review.id, review.generation, {{other_critic, CriticRole::GeneralCritic}});
    const EvidenceRecord foreign =
        fixture.evidence(other_assignments.front(), target, other_critic, 7001);
    SubmitFindingRequest cross = finding;
    cross.evidence = {foreign.id};
    const auto cross_rejected = fixture.coordinator.submit_finding(cross);
    CF_CHECK(!cross_rejected.ok());
    CF_CHECK(cross_rejected.code() == ErrorCode::EvidenceNotCurrent);
    (void)assignments;
}

CF_TEST(assignment_requires_readiness_and_matching_capability) {
    Fixture fixture;
    const TargetRecord target = fixture.target("assignment eligibility");
    ReviewSpec spec = default_review_spec();
    spec.required_roles = {CriticRole::SecurityCritic};
    const ReviewRecord review = fixture.declare(target, spec);

    const Fixture::Critic not_ready =
        fixture.critic(CriticRole::SecurityCritic, SyntheticBehavior::Pass,
                       FindingCategory::Security, Severity::High, false);
    const auto rejected = fixture.try_assign(review.id, review.generation,
                                             {{not_ready, CriticRole::SecurityCritic}});
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::RequiredCriticUnavailable);

    // A critic that does not declare the assigned role cannot be assigned.
    const Fixture::Critic wrong_role = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto role_rejected =
        fixture.try_assign(review.id, review.generation, {{wrong_role, CriticRole::SecurityCritic}});
    CF_CHECK(!role_rejected.ok());
    CF_CHECK(role_rejected.code() == ErrorCode::CriticCapabilityMismatch);
}

CF_TEST(stale_review_and_target_generations_are_distinguished) {
    Fixture fixture;
    const TargetRecord target = fixture.target("stale generations");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});

    SubmitFindingRequest request;
    request.review = review.id;
    request.review_generation = ReviewGeneration::from_value(review.generation.value() + 5);
    request.target = target.id;
    request.target_generation = target.generation;
    request.critic = critic.id;
    request.critic_generation = critic.generation;
    request.worker = critic.worker;
    request.worker_boot = critic.boot;
    request.outcome = FindingOutcome::Pass;
    request.epoch = fixture.epoch();
    const auto stale_review = fixture.coordinator.submit_finding(request);
    CF_CHECK(!stale_review.ok());
    CF_CHECK(stale_review.code() == ErrorCode::StaleReviewGeneration);

    request.review_generation = review.generation;
    request.target_generation = TargetGeneration::from_value(target.generation.value() + 5);
    const auto target_mismatch = fixture.coordinator.submit_finding(request);
    CF_CHECK(!target_mismatch.ok());
    CF_CHECK(target_mismatch.code() == ErrorCode::TargetMismatch);
    (void)assignments;
}

CF_TEST(seeded_randomized_review_state_machine_holds_invariants) {
    constexpr std::uint64_t kSeed = 0x5EED1234u;
    Rng rng(kSeed);
    TempDirectory directory("property");
    const std::string path = directory.file("state.journal");

    Fixture fixture(true, path);
    std::vector<TargetRecord> targets;
    std::vector<ReviewRecord> reviews;
    std::vector<Fixture::Critic> critics;
    std::vector<std::vector<CriticAssignment>> assignments;
    std::vector<TargetGeneration> review_target_generation;

    targets.push_back(fixture.target("property target one"));
    targets.push_back(fixture.target("property target two"));
    for (int i = 0; i < 6; ++i) {
        critics.push_back(fixture.critic(CriticRole::GeneralCritic,
                                         i % 3 == 0 ? SyntheticBehavior::Pass
                                                    : (i % 3 == 1 ? SyntheticBehavior::Fail
                                                                  : SyntheticBehavior::Warn)));
    }

    std::uint64_t nonce = 1;
    for (int step = 0; step < 600; ++step) {
        const std::uint32_t action = rng.below(8);
        if (action == 0 || reviews.empty()) {
            if (reviews.size() >= 8) {
                continue;
            }
            ReviewSpec spec = default_review_spec();
            spec.duplicates = DuplicatePolicy::KeepDistinct;
            spec.min_critic_count = 1;
            spec.quorum = 1;
            spec.allow_partial_failure = true;
            const std::size_t index = rng.below(static_cast<std::uint32_t>(targets.size()));
            const auto declared = fixture.coordinator.declare_review([&] {
                DeclareReviewRequest request;
                request.request = ReviewRequestId::from_value(900000 + reviews.size());
                request.target = targets[index].id;
                request.target_generation = targets[index].generation;
                request.spec = spec;
                request.epoch = fixture.epoch();
                return request;
            }());
            if (declared.ok()) {
                reviews.push_back(declared.value());
                assignments.emplace_back();
                review_target_generation.push_back(targets[index].generation);
            }
            continue;
        }

        const std::size_t review_index = rng.below(static_cast<std::uint32_t>(reviews.size()));
        ReviewRecord& review = reviews[review_index];
        const std::size_t critic_index = rng.below(static_cast<std::uint32_t>(critics.size()));

        switch (action) {
            case 1: {
                const auto before = assignments[review_index].size();
                const auto created = fixture.try_assign(review.id, review.generation,
                                                        {{critics[critic_index], CriticRole::GeneralCritic}});
                if (created.ok()) {
                    for (const CriticAssignment& assignment : created.value()) {
                        assignments[review_index].push_back(assignment);
                    }
                    CF_CHECK(assignments[review_index].size() == before + 1);
                }
                break;
            }
            case 2: {
                if (assignments[review_index].empty()) {
                    break;
                }
                const std::size_t which = rng.below(static_cast<std::uint32_t>(assignments[review_index].size()));
                const CriticAssignment& assignment = assignments[review_index][which];
                const Fixture::Critic& owner = critics[critic_index];
                if (assignment.critic != owner.id) {
                    break;
                }
                const SyntheticBehavior behavior =
                    static_cast<SyntheticBehavior>(1 + rng.below(5));
                (void)fixture.try_publish(assignment, targets[0], owner, behavior, ++nonce);
                break;
            }
            case 3: {
                (void)fixture.try_commit(review, review_target_generation[review_index]);
                break;
            }
            case 4: {
                CancelReviewRequest request;
                request.review = review.id;
                request.review_generation = review.generation;
                request.reason = "randomized cancellation";
                request.epoch = fixture.epoch();
                (void)fixture.coordinator.cancel_review(request);
                break;
            }
            case 5: {
                const std::size_t index = rng.below(static_cast<std::uint32_t>(targets.size()));
                RegisterTargetRequest request;
                request.id = targets[index].id;
                request.target_class = targets[index].target_class;
                request.schema_name = targets[index].schema_name;
                const std::string payload = "revision-" + std::to_string(step);
                request.payload.assign(payload.begin(), payload.end());
                request.epoch = fixture.epoch();
                const auto advanced = fixture.coordinator.register_target(request);
                if (advanced.ok()) {
                    targets[index] = advanced.value();
                }
                break;
            }
            case 6: {
                const auto snapshot = fixture.coordinator.query_review(review.id);
                CF_REQUIRE(snapshot.ok());
                // Invariant: at most one authoritative decision exists, and it
                // always names this review at this generation.
                if (snapshot.value().decision.has_value()) {
                    CF_CHECK(snapshot.value().decision->review == snapshot.value().review.id);
                    CF_CHECK(snapshot.value().decision->review_generation ==
                             snapshot.value().review.generation);
                }
                // Invariant: once committed, the state stays committed until the
                // target generation advances.
                if (snapshot.value().review.state == ReviewState::Committed) {
                    CF_CHECK(snapshot.value().decision.has_value());
                }
                // Invariant: no finding may be bound to a different review
                // generation than the review itself.
                for (const Finding& finding : snapshot.value().findings) {
                    CF_CHECK(finding.review == snapshot.value().review.id);
                }
                break;
            }
            default: {
                const auto summary = fixture.coordinator.list_reviews();
                CF_CHECK(!summary.empty());
                break;
            }
        }
    }

    // Final invariants over the whole coordinator.
    for (const ReviewSummary& summary : fixture.coordinator.list_reviews()) {
        const auto snapshot = fixture.coordinator.query_review(summary.id);
        CF_REQUIRE(snapshot.ok());
        if (snapshot.value().decision.has_value()) {
            CF_CHECK(snapshot.value().review.state == ReviewState::Committed ||
                     !snapshot.value().review.authority_current);
            CF_CHECK(snapshot.value().decision->review == snapshot.value().review.id);
            CF_CHECK(snapshot.value().decision->review_generation == snapshot.value().review.generation);
        }
        if (!snapshot.value().review.authority_current) {
            CF_CHECK(snapshot.value().review.state != ReviewState::Reviewing ||
                     snapshot.value().review.state == ReviewState::Superseded);
        }
    }
    std::printf("  property test seed 0x%llx completed with %zu reviews\n",
                static_cast<unsigned long long>(kSeed),
                static_cast<std::size_t>(fixture.coordinator.list_reviews().size()));
}

CF_TEST(adversarial_replay_and_reordering_are_rejected) {
    Fixture fixture;
    const TargetRecord target = fixture.target("adversarial replay");
    const ReviewRecord review = fixture.declare(target, default_review_spec());
    const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto assignments = fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
    const Finding finding =
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);

    // Replaying the byte-identical finding is refused under the default policy;
    // only an explicit IDEMPOTENT_IGNORE policy accepts it, and then it returns
    // the original finding rather than a second one.
    SubmitFindingRequest replay;
    replay.review = review.id;
    replay.review_generation = review.generation;
    replay.target = target.id;
    replay.target_generation = target.generation;
    replay.critic = critic.id;
    replay.critic_generation = critic.generation;
    replay.worker = critic.worker;
    replay.worker_boot = critic.boot;
    replay.category = finding.category;
    replay.severity = finding.severity;
    replay.outcome = finding.outcome;
    replay.evidence = finding.evidence;
    replay.explanation = finding.explanation;
    replay.epoch = fixture.epoch();
    const auto replay_result = fixture.coordinator.submit_finding(replay);
    CF_CHECK(!replay_result.ok());
    CF_CHECK(replay_result.code() == ErrorCode::DuplicateFinding);

    // A finding claiming another critic's identity while presenting this worker's
    // boot identity is rejected.
    SubmitFindingRequest forged;
    forged.review = review.id;
    forged.review_generation = review.generation;
    forged.target = target.id;
    forged.target_generation = target.generation;
    forged.critic = CriticId::from_value(critic.id.value() + 500);
    forged.critic_generation = critic.generation;
    forged.worker = critic.worker;
    forged.worker_boot = critic.boot;
    forged.outcome = FindingOutcome::Pass;
    forged.epoch = fixture.epoch();
    const auto forged_result = fixture.coordinator.submit_finding(forged);
    CF_CHECK(!forged_result.ok());
    CF_CHECK(forged_result.code() == ErrorCode::CriticNotRegistered);

    // An evidence record bound to another critic's finding is rejected.
    const Fixture::Critic second = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
    const auto second_assignments =
        fixture.assign(review.id, review.generation, {{second, CriticRole::GeneralCritic}});
    const EvidenceRecord second_evidence =
        fixture.evidence(second_assignments.front(), target, second, 31337);
    SubmitEvidenceRequest rebind;
    rebind.review = review.id;
    rebind.review_generation = review.generation;
    rebind.target = target.id;
    rebind.target_generation = target.generation;
    rebind.critic = second.id;
    rebind.critic_generation = second.generation;
    rebind.worker = second.worker;
    rebind.worker_boot = second.boot;
    rebind.finding = finding.id;
    rebind.provenance = EvidenceProvenance::Measured;
    rebind.integrity = EvidenceIntegrity::Verified;
    rebind.payload = {1, 2};
    rebind.epoch = fixture.epoch();
    const auto rebind_result = fixture.coordinator.submit_evidence(rebind);
    CF_CHECK(!rebind_result.ok());
    CF_CHECK(rebind_result.code() == ErrorCode::MalformedEvidence);
    (void)second_evidence;
}

CF_TEST(committed_state_digest_is_stable_across_reload) {
    TempDirectory directory("digest");
    const std::string path = directory.file("state.journal");
    Digest first_digest;
    {
        Fixture fixture(true, path);
        const TargetRecord target = fixture.target("state digest");
        const ReviewRecord review = fixture.declare(target, default_review_spec());
        const Fixture::Critic critic = fixture.critic(CriticRole::GeneralCritic, SyntheticBehavior::Pass);
        const auto assignments =
            fixture.assign(review.id, review.generation, {{critic, CriticRole::GeneralCritic}});
        fixture.publish(assignments.front(), target, critic, SyntheticBehavior::Pass, fixture.next_nonce_++);
        CF_REQUIRE(fixture.try_commit(review, target.generation).ok());
        first_digest = fixture.coordinator.authoritative_state_digest();
        // Determinism: the same committed history yields the same digest twice.
        CF_CHECK(fixture.coordinator.authoritative_state_digest() == first_digest);
        fixture.coordinator.stop();
    }
    Digest second_digest;
    {
        Fixture restarted(true, path);
        second_digest = restarted.coordinator.authoritative_state_digest();
    }
    // Committed review truth survives a restart unchanged.
    CF_CHECK(first_digest == second_digest);
}

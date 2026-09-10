// Critic Fabric — inspection CLI.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The inspection tool is not the source of truth. It is a read-only surface over
// persisted state or a live coordinator.
//
// Usage:
//   critic_inspect --persistence PATH [--review RID] [--verbose]
//   critic_inspect --port N [--review RID] [--workers]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/platform.hpp"
#include "critic_fabric/transport.hpp"

namespace {

std::map<std::string, std::string> parse_flags(int argc, char** argv) {
    std::map<std::string, std::string> flags;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--", 0) != 0) {
            continue;
        }
        const std::string key = argument.substr(2);
        if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
            flags[key] = argv[++i];
        } else {
            flags[key] = "1";
        }
    }
    return flags;
}

void print_finding(const critic_fabric::Finding& finding) {
    using namespace critic_fabric;
    std::printf("  finding #%llu generation %llu critic %llu generation %llu state %s outcome %s "
                "severity %s category %s attempt %llu evidence %zu\n",
                static_cast<unsigned long long>(finding.id.value()),
                static_cast<unsigned long long>(finding.generation.value()),
                static_cast<unsigned long long>(finding.critic.value()),
                static_cast<unsigned long long>(finding.critic_generation.value()),
                to_string(finding.state), to_string(finding.outcome), to_string(finding.severity),
                to_string(finding.category),
                static_cast<unsigned long long>(finding.attempt.value()), finding.evidence.size());
    std::printf("    worker %llu boot %llu review %llu/%llu target %llu/%llu digest %s\n",
                static_cast<unsigned long long>(finding.worker.value()),
                static_cast<unsigned long long>(finding.worker_boot.value()),
                static_cast<unsigned long long>(finding.review.value()),
                static_cast<unsigned long long>(finding.review_generation.value()),
                static_cast<unsigned long long>(finding.target.value()),
                static_cast<unsigned long long>(finding.target_generation.value()),
                finding.finding_digest.to_hex().substr(0, 16).c_str());
    if (!finding.explanation.empty()) {
        std::printf("    explanation: %s\n", finding.explanation.c_str());
    }
    std::printf("    confidence authority: none (advisory metadata only; provided=%d)\n",
                finding.confidence.provided ? 1 : 0);
}

int inspect_snapshot(const critic_fabric::ReviewSnapshot& snapshot, bool verbose) {
    using namespace critic_fabric;
    std::printf("review #%llu generation %llu\n",
                static_cast<unsigned long long>(snapshot.review.id.value()),
                static_cast<unsigned long long>(snapshot.review.generation.value()));
    std::printf("  state %s disposition %s authority_current %d rounds %u\n",
                to_string(snapshot.review.state), to_string(snapshot.review.disposition),
                snapshot.review.authority_current ? 1 : 0, snapshot.review.rounds_used);
    std::printf("  target #%llu generation %llu digest %s\n",
                static_cast<unsigned long long>(snapshot.review.target.value()),
                static_cast<unsigned long long>(snapshot.review.target_generation.value()),
                snapshot.review.target_digest.to_hex().c_str());
    std::printf("  policy: %s\n", snapshot.review.spec.summarize().c_str());
    std::printf("  policy_digest %s\n", snapshot.review.spec.compute_policy_digest().to_hex().c_str());
    if (!snapshot.review.status_detail.empty()) {
        std::printf("  status: %s\n", snapshot.review.status_detail.c_str());
    }
    if (!snapshot.review.cancellation_reason.empty()) {
        std::printf("  cancellation_reason: %s\n", snapshot.review.cancellation_reason.c_str());
    }
    if (snapshot.review.superseded_by.valid()) {
        std::printf("  superseded_by generation %llu\n",
                    static_cast<unsigned long long>(snapshot.review.superseded_by.value()));
    }

    std::printf("  assignments (%zu)\n", snapshot.assignments.size());
    for (const CriticAssignment& assignment : snapshot.assignments) {
        std::printf("    #%llu critic %llu generation %llu worker %llu boot %llu role %s required %d "
                    "weight %u active %d%s%s\n",
                    static_cast<unsigned long long>(assignment.id.value()),
                    static_cast<unsigned long long>(assignment.critic.value()),
                    static_cast<unsigned long long>(assignment.critic_generation.value()),
                    static_cast<unsigned long long>(assignment.worker.value()),
                    static_cast<unsigned long long>(assignment.worker_boot.value()),
                    to_string(assignment.role), assignment.required ? 1 : 0, assignment.weight,
                    assignment.active ? 1 : 0,
                    assignment.retirement_reason.empty() ? "" : " retired: ",
                    assignment.retirement_reason.c_str());
    }

    std::printf("  findings (%zu)\n", snapshot.findings.size());
    for (const Finding& finding : snapshot.findings) {
        print_finding(finding);
    }

    std::printf("  evidence (%zu)\n", snapshot.evidences.size());
    for (const EvidenceRecord& evidence : snapshot.evidences) {
        std::printf("    #%llu source %s provenance %s integrity %s current %d superseded %d digest %s\n",
                    static_cast<unsigned long long>(evidence.id.value()),
                    to_string(evidence.source_type), to_string(evidence.provenance),
                    to_string(evidence.integrity), evidence.current ? 1 : 0, evidence.superseded ? 1 : 0,
                    evidence.content_digest.to_hex().c_str());
        if (verbose && !evidence.reference.empty()) {
            std::printf("      reference: %s\n", evidence.reference.c_str());
        }
    }

    if (!snapshot.challenges.empty()) {
        std::printf("  challenges (%zu)\n", snapshot.challenges.size());
        for (const Challenge& challenge : snapshot.challenges) {
            std::printf("    #%llu generation %llu finding %llu challenger %llu outcome %s resolved %d\n",
                        static_cast<unsigned long long>(challenge.id.value()),
                        static_cast<unsigned long long>(challenge.generation.value()),
                        static_cast<unsigned long long>(challenge.challenged_finding.value()),
                        static_cast<unsigned long long>(challenge.challenger.value()),
                        to_string(challenge.outcome), challenge.resolved ? 1 : 0);
            if (verbose) {
                std::printf("      disputed: %s\n", challenge.disputed_predicate.c_str());
                std::printf("      rationale: %s\n", challenge.rationale.c_str());
            }
        }
    }
    if (!snapshot.rebuttals.empty()) {
        std::printf("  rebuttals (%zu)\n", snapshot.rebuttals.size());
        for (const Rebuttal& rebuttal : snapshot.rebuttals) {
            std::printf("    #%llu challenge %llu author %llu\n",
                        static_cast<unsigned long long>(rebuttal.id.value()),
                        static_cast<unsigned long long>(rebuttal.challenge.value()),
                        static_cast<unsigned long long>(rebuttal.author.value()));
            if (verbose) {
                std::printf("      argument: %s\n", rebuttal.argument.c_str());
            }
        }
    }

    if (snapshot.decision.has_value()) {
        std::printf("  decision #%llu generation %llu disposition %s epoch %llu\n",
                    static_cast<unsigned long long>(snapshot.decision->id.value()),
                    static_cast<unsigned long long>(snapshot.decision->generation.value()),
                    to_string(snapshot.decision->disposition),
                    static_cast<unsigned long long>(snapshot.decision->epoch.value()));
        std::printf("    decision_digest %s\n", snapshot.decision->decision_digest.to_hex().c_str());
        std::printf("    policy_digest %s\n", snapshot.decision->policy_digest.to_hex().c_str());
        std::printf("    evidence_basis_digest %s\n",
                    snapshot.decision->evidence_basis_digest.to_hex().c_str());
    } else {
        std::printf("  decision: none committed for this review generation\n");
    }
    std::printf("  state_digest %s\n", snapshot.state_digest.to_hex().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    critic_fabric::platform::configure_process_for_tests();
    using namespace critic_fabric;
    const auto flags = parse_flags(argc, argv);
    const auto persistence = flags.find("persistence");
    const auto port = flags.find("port");
    const bool verbose = flags.count("verbose") != 0;

    if (persistence == flags.end() && port == flags.end()) {
        std::fprintf(stderr, "usage: critic_inspect (--persistence PATH | --port N) [--review RID] [--verbose]\n");
        return 2;
    }

    if (persistence != flags.end()) {
        CoordinatorConfig config;
        config.persistence_path = persistence->second;
        config.process_label = "critic_inspect";
        Coordinator coordinator(config);
        Status status = coordinator.start();
        if (!status.ok()) {
            std::fprintf(stderr, "unable to load persisted state: %s\n", status.to_string().c_str());
            return 1;
        }
        const RecoveryReport report = coordinator.recovery_report();
        std::printf("durable journal %s\n", persistence->second.c_str());
        std::printf("  epoch %llu records %llu\n",
                    static_cast<unsigned long long>(report.epoch.value()),
                    static_cast<unsigned long long>(report.journal_records));
        std::printf("  recovered: targets %u critics %u workers %u reviews %u findings %u decisions %u\n",
                    report.targets_recovered, report.critics_recovered, report.workers_recovered,
                    report.reviews_recovered, report.findings_recovered, report.decisions_recovered);
        std::printf("  dynamic authority cleared %u; reviews moved to revalidation %u\n",
                    report.dynamic_authority_cleared, report.reviews_moved_to_revalidation);
        std::printf("  authoritative state digest %s\n",
                    coordinator.authoritative_state_digest().to_hex().c_str());

        if (flags.count("review") != 0) {
            const ReviewId review =
                ReviewId::from_value(std::strtoull(flags.at("review").c_str(), nullptr, 10));
            Result<ReviewSnapshot> snapshot = coordinator.query_review(review);
            if (!snapshot.ok()) {
                std::fprintf(stderr, "review query rejected: %s\n", snapshot.status().to_string().c_str());
                return 1;
            }
            return inspect_snapshot(snapshot.value(), verbose);
        }

        for (const ReviewSummary& summary : coordinator.list_reviews()) {
            std::printf("review #%llu generation %llu target %llu/%llu state %s disposition %s "
                        "findings %u current %u assignments %u decision %d\n",
                        static_cast<unsigned long long>(summary.id.value()),
                        static_cast<unsigned long long>(summary.generation.value()),
                        static_cast<unsigned long long>(summary.target.value()),
                        static_cast<unsigned long long>(summary.target_generation.value()),
                        to_string(summary.state), to_string(summary.disposition), summary.finding_count,
                        summary.current_finding_count, summary.assignment_count,
                        summary.has_decision ? 1 : 0);
        }
        for (const WorkerRecord& record : coordinator.list_workers()) {
            std::printf("worker #%llu boot %llu critic %llu generation %llu ready %d healthy %d "
                        "authoritative %d label %s\n",
                        static_cast<unsigned long long>(record.id.value()),
                        static_cast<unsigned long long>(record.boot.value()),
                        static_cast<unsigned long long>(record.critic.value()),
                        static_cast<unsigned long long>(record.critic_generation.value()),
                        record.ready ? 1 : 0, record.healthy ? 1 : 0, record.authoritative ? 1 : 0,
                        record.process_label.c_str());
        }
        return 0;
    }

    CoordinatorClient client;
    RuntimeLimits limits;
    Status status = client.connect("127.0.0.1",
                                   static_cast<std::uint16_t>(std::strtoul(port->second.c_str(), nullptr, 10)),
                                   limits);
    if (!status.ok()) {
        std::fprintf(stderr, "unable to connect: %s\n", status.to_string().c_str());
        return 1;
    }
    status = client.hello(RequestId::from_value(1));
    if (!status.ok()) {
        std::fprintf(stderr, "handshake rejected: %s\n", status.to_string().c_str());
        return 1;
    }
    std::printf("live coordinator epoch %llu\n",
                static_cast<unsigned long long>(client.epoch().value()));

    if (flags.count("review") != 0) {
        const ReviewId review = ReviewId::from_value(std::strtoull(flags.at("review").c_str(), nullptr, 10));
        Result<ReviewSnapshot> snapshot = client.query_review(review, ReviewGeneration{});
        if (!snapshot.ok()) {
            std::fprintf(stderr, "review query rejected: %s\n", snapshot.status().to_string().c_str());
            return 1;
        }
        return inspect_snapshot(snapshot.value(), verbose);
    }

    Result<std::vector<ReviewSummary>> reviews = client.list_reviews();
    if (reviews.ok()) {
        for (const ReviewSummary& summary : reviews.value()) {
            std::printf("review #%llu generation %llu state %s disposition %s findings %u current %u\n",
                        static_cast<unsigned long long>(summary.id.value()),
                        static_cast<unsigned long long>(summary.generation.value()),
                        to_string(summary.state), to_string(summary.disposition),
                        summary.finding_count, summary.current_finding_count);
        }
    }
    Result<std::vector<WorkerRecord>> workers = client.list_workers();
    if (workers.ok()) {
        for (const WorkerRecord& record : workers.value()) {
            std::printf("worker #%llu boot %llu critic %llu ready %d healthy %d authoritative %d\n",
                        static_cast<unsigned long long>(record.id.value()),
                        static_cast<unsigned long long>(record.boot.value()),
                        static_cast<unsigned long long>(record.critic.value()), record.ready ? 1 : 0,
                        record.healthy ? 1 : 0, record.authoritative ? 1 : 0);
        }
    }
    return 0;
}

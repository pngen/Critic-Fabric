// Critic Fabric — controller / client tool.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A thin control surface over a live coordinator. It never mutates state that
// the coordinator does not authorize, and it prints the typed rejection it
// receives instead of guessing.
//
// Usage:
//   critic_controller --port N target-create --id ID --class CLAIM --schema NAME --payload TEXT
//   critic_controller --port N target-show --id ID
//   critic_controller --port N review-declare --request ID --target TID --generation G
//   critic_controller --port N review-assign --review RID --generation G --critic CID \
//                     --worker WID --boot BID --role ROLE
//   critic_controller --port N review-commit --review RID --generation G
//   critic_controller --port N review-cancel --review RID --generation G --reason TEXT
//   critic_controller --port N review-show --review RID
//   critic_controller --port N decision-show --review RID --generation G
//   critic_controller --port N list-reviews
//   critic_controller --port N list-workers

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "critic_fabric/platform.hpp"
#include "critic_fabric/transport.hpp"

namespace {

struct Arguments {
    std::string command;
    std::map<std::string, std::string> flags;
};

// Flags take an optional value. The first token that is not consumed as a flag
// value is the command, so "--port 9000 review-show --review 1" and
// "review-show --port 9000 --review 1" are both accepted.
Arguments parse_arguments(int argc, char** argv) {
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--", 0) == 0) {
            const std::string key = argument.substr(2);
            if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
                arguments.flags[key] = argv[++i];
            } else {
                arguments.flags[key] = "1";
            }
            continue;
        }
        if (arguments.command.empty()) {
            arguments.command = argument;
        }
    }
    return arguments;
}

std::uint64_t number(const std::map<std::string, std::string>& flags, const char* key,
                     std::uint64_t fallback = 0) {
    const auto it = flags.find(key);
    if (it == flags.end()) {
        return fallback;
    }
    return std::strtoull(it->second.c_str(), nullptr, 10);
}

std::string text(const std::map<std::string, std::string>& flags, const char* key) {
    const auto it = flags.find(key);
    return it == flags.end() ? std::string() : it->second;
}

int report(const critic_fabric::Status& status) {
    std::fprintf(stderr, "rejected: %s\n", status.to_string().c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    critic_fabric::platform::configure_process_for_tests();
    using namespace critic_fabric;

    if (argc < 3) {
        std::fprintf(stderr, "usage: critic_controller --port N <command> [--flag value ...]\n");
        return 2;
    }
    const Arguments parsed = parse_arguments(argc, argv);
    const auto& flags = parsed.flags;
    const auto port_it = flags.find("port");
    if (port_it == flags.end()) {
        std::fprintf(stderr, "--port is required\n");
        return 2;
    }
    const std::string command = parsed.command;
    if (command.empty()) {
        std::fprintf(stderr, "no command supplied\n");
        return 2;
    }

    CoordinatorClient client;
    RuntimeLimits limits;
    Status status = client.connect(text(flags, "host").empty() ? "127.0.0.1" : text(flags, "host"),
                                   static_cast<std::uint16_t>(std::strtoul(port_it->second.c_str(), nullptr, 10)),
                                   limits);
    if (!status.ok()) {
        return report(status);
    }
    status = client.hello(RequestId::from_value(1));
    if (!status.ok()) {
        return report(status);
    }
    const CoordinatorEpoch epoch = client.epoch();
    std::printf("epoch=%llu\n", static_cast<unsigned long long>(epoch.value()));

    if (command == "target-create") {
        RegisterTargetRequest request;
        request.id = TargetId::from_value(number(flags, "id"));
        TargetClass target_class = TargetClass::Claim;
        parse_target_class(text(flags, "class").empty() ? "CLAIM" : text(flags, "class"), target_class);
        request.target_class = target_class;
        request.schema_name = text(flags, "schema");
        const std::string payload = text(flags, "payload");
        request.payload.assign(payload.begin(), payload.end());
        request.provenance = "critic_controller";
        request.epoch = epoch;
        request.request = RequestId::from_value(2);
        Result<TargetRecord> result = client.register_target(request);
        if (!result.ok()) {
            return report(result.status());
        }
        std::printf("target=%llu generation=%llu digest=%s reviewable=%d\n",
                    static_cast<unsigned long long>(result.value().id.value()),
                    static_cast<unsigned long long>(result.value().generation.value()),
                    result.value().content_digest.to_hex().c_str(),
                    result.value().reviewable ? 1 : 0);
        return 0;
    }

    if (command == "target-show") {
        Result<TargetRecord> result = client.query_target(TargetId::from_value(number(flags, "id")));
        if (!result.ok()) {
            return report(result.status());
        }
        std::printf("target=%llu generation=%llu class=%s digest=%s supersession=%s\n",
                    static_cast<unsigned long long>(result.value().id.value()),
                    static_cast<unsigned long long>(result.value().generation.value()),
                    to_string(result.value().target_class),
                    result.value().content_digest.to_hex().c_str(),
                    to_string(result.value().supersession));
        return 0;
    }

    if (command == "review-declare") {
        DeclareReviewRequest request;
        request.request = ReviewRequestId::from_value(number(flags, "request"));
        request.target = TargetId::from_value(number(flags, "target"));
        request.target_generation = TargetGeneration::from_value(number(flags, "generation", 1));
        request.spec = default_review_spec();
        request.epoch = epoch;
        Result<ReviewRecord> result = client.declare_review(request);
        if (!result.ok()) {
            return report(result.status());
        }
        std::printf("review=%llu generation=%llu state=%s\n",
                    static_cast<unsigned long long>(result.value().id.value()),
                    static_cast<unsigned long long>(result.value().generation.value()),
                    to_string(result.value().state));
        return 0;
    }

    if (command == "review-assign") {
        AssignCriticsRequest request;
        request.review = ReviewId::from_value(number(flags, "review"));
        request.review_generation = ReviewGeneration::from_value(number(flags, "generation", 1));
        AssignCriticsRequest::Assignment assignment;
        assignment.critic = CriticId::from_value(number(flags, "critic"));
        assignment.worker = WorkerId::from_value(number(flags, "worker"));
        assignment.worker_boot = WorkerBootId::from_value(number(flags, "boot"));
        assignment.critic_generation = CriticGeneration::from_value(number(flags, "critic-generation", 1));
        CriticRole role = CriticRole::GeneralCritic;
        parse_critic_role(text(flags, "role").empty() ? "GENERAL_CRITIC" : text(flags, "role"), role);
        assignment.role = role;
        request.assignments.push_back(assignment);
        request.epoch = epoch;
        request.request = RequestId::from_value(3);
        Result<std::vector<CriticAssignment>> result = client.assign_critics(request);
        if (!result.ok()) {
            return report(result.status());
        }
        std::printf("assignments=%zu\n", result.value().size());
        return 0;
    }

    if (command == "review-commit") {
        CommitReviewRequest request;
        request.review = ReviewId::from_value(number(flags, "review"));
        request.review_generation = ReviewGeneration::from_value(number(flags, "generation", 1));
        request.target_generation = TargetGeneration::from_value(number(flags, "target-generation", 1));
        request.epoch = epoch;
        request.request = RequestId::from_value(4);
        Result<Decision> result = client.commit_review(request);
        if (!result.ok()) {
            return report(result.status());
        }
        std::printf("decision=%llu generation=%llu disposition=%s digest=%s\n",
                    static_cast<unsigned long long>(result.value().id.value()),
                    static_cast<unsigned long long>(result.value().generation.value()),
                    to_string(result.value().disposition),
                    result.value().decision_digest.to_hex().c_str());
        return 0;
    }

    if (command == "review-cancel") {
        CancelReviewRequest request;
        request.review = ReviewId::from_value(number(flags, "review"));
        request.review_generation = ReviewGeneration::from_value(number(flags, "generation", 1));
        request.reason = text(flags, "reason");
        request.epoch = epoch;
        request.request = RequestId::from_value(5);
        Result<ReviewRecord> result = client.cancel_review(request);
        if (!result.ok()) {
            return report(result.status());
        }
        std::printf("review=%llu state=%s\n",
                    static_cast<unsigned long long>(result.value().id.value()),
                    to_string(result.value().state));
        return 0;
    }

    if (command == "review-show") {
        Result<ReviewSnapshot> result =
            client.query_review(ReviewId::from_value(number(flags, "review")), ReviewGeneration{});
        if (!result.ok()) {
            return report(result.status());
        }
        const ReviewSnapshot& snapshot = result.value();
        std::printf("review=%llu generation=%llu state=%s disposition=%s authority_current=%d\n",
                    static_cast<unsigned long long>(snapshot.review.id.value()),
                    static_cast<unsigned long long>(snapshot.review.generation.value()),
                    to_string(snapshot.review.state), to_string(snapshot.review.disposition),
                    snapshot.review.authority_current ? 1 : 0);
        std::printf("target=%llu target_generation=%llu digest=%s\n",
                    static_cast<unsigned long long>(snapshot.review.target.value()),
                    static_cast<unsigned long long>(snapshot.review.target_generation.value()),
                    snapshot.review.target_digest.to_hex().c_str());
        for (const CriticAssignment& assignment : snapshot.assignments) {
            std::printf("  assignment=%llu critic=%llu generation=%llu worker=%llu boot=%llu role=%s "
                        "required=%d active=%d\n",
                        static_cast<unsigned long long>(assignment.id.value()),
                        static_cast<unsigned long long>(assignment.critic.value()),
                        static_cast<unsigned long long>(assignment.critic_generation.value()),
                        static_cast<unsigned long long>(assignment.worker.value()),
                        static_cast<unsigned long long>(assignment.worker_boot.value()),
                        to_string(assignment.role), assignment.required ? 1 : 0, assignment.active ? 1 : 0);
        }
        for (const Finding& finding : snapshot.findings) {
            std::printf("  finding=%llu generation=%llu critic=%llu state=%s outcome=%s severity=%s "
                        "category=%s evidence=%zu\n",
                        static_cast<unsigned long long>(finding.id.value()),
                        static_cast<unsigned long long>(finding.generation.value()),
                        static_cast<unsigned long long>(finding.critic.value()),
                        to_string(finding.state), to_string(finding.outcome), to_string(finding.severity),
                        to_string(finding.category), finding.evidence.size());
        }
        for (const EvidenceRecord& evidence : snapshot.evidences) {
            std::printf("  evidence=%llu provenance=%s integrity=%s current=%d digest=%s\n",
                        static_cast<unsigned long long>(evidence.id.value()),
                        to_string(evidence.provenance), to_string(evidence.integrity),
                        evidence.current ? 1 : 0, evidence.content_digest.to_hex().c_str());
        }
        for (const Challenge& challenge : snapshot.challenges) {
            std::printf("  challenge=%llu finding=%llu outcome=%s resolved=%d\n",
                        static_cast<unsigned long long>(challenge.id.value()),
                        static_cast<unsigned long long>(challenge.challenged_finding.value()),
                        to_string(challenge.outcome), challenge.resolved ? 1 : 0);
        }
        if (snapshot.decision.has_value()) {
            std::printf("  decision=%llu disposition=%s digest=%s\n",
                        static_cast<unsigned long long>(snapshot.decision->id.value()),
                        to_string(snapshot.decision->disposition),
                        snapshot.decision->decision_digest.to_hex().c_str());
        }
        std::printf("state_digest=%s\n", snapshot.state_digest.to_hex().c_str());
        return 0;
    }

    if (command == "decision-show") {
        Result<Decision> result = client.query_decision(
            ReviewId::from_value(number(flags, "review")),
            ReviewGeneration::from_value(number(flags, "generation", 1)));
        if (!result.ok()) {
            return report(result.status());
        }
        std::printf("decision=%llu disposition=%s epoch=%llu digest=%s\n",
                    static_cast<unsigned long long>(result.value().id.value()),
                    to_string(result.value().disposition),
                    static_cast<unsigned long long>(result.value().epoch.value()),
                    result.value().decision_digest.to_hex().c_str());
        return 0;
    }

    if (command == "list-reviews") {
        Result<std::vector<ReviewSummary>> result = client.list_reviews();
        if (!result.ok()) {
            return report(result.status());
        }
        for (const ReviewSummary& summary : result.value()) {
            std::printf("review=%llu generation=%llu target=%llu target_generation=%llu state=%s "
                        "disposition=%s findings=%u current=%u assignments=%u decision=%d\n",
                        static_cast<unsigned long long>(summary.id.value()),
                        static_cast<unsigned long long>(summary.generation.value()),
                        static_cast<unsigned long long>(summary.target.value()),
                        static_cast<unsigned long long>(summary.target_generation.value()),
                        to_string(summary.state), to_string(summary.disposition), summary.finding_count,
                        summary.current_finding_count, summary.assignment_count,
                        summary.has_decision ? 1 : 0);
        }
        return 0;
    }

    if (command == "list-workers") {
        Result<std::vector<WorkerRecord>> result = client.list_workers();
        if (!result.ok()) {
            return report(result.status());
        }
        for (const WorkerRecord& record : result.value()) {
            std::printf("worker=%llu boot=%llu critic=%llu generation=%llu ready=%d healthy=%d "
                        "authoritative=%d boot_order=%llu label=%s\n",
                        static_cast<unsigned long long>(record.id.value()),
                        static_cast<unsigned long long>(record.boot.value()),
                        static_cast<unsigned long long>(record.critic.value()),
                        static_cast<unsigned long long>(record.critic_generation.value()),
                        record.ready ? 1 : 0, record.healthy ? 1 : 0, record.authoritative ? 1 : 0,
                        static_cast<unsigned long long>(record.boot_order), record.process_label.c_str());
        }
        return 0;
    }

    std::fprintf(stderr, "unknown command: %s\n", command.c_str());
    return 2;
}

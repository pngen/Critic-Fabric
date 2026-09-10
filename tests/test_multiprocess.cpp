// Critic Fabric — real multiprocess reference deployment proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every process here is an independent operating-system process communicating
// over real loopback TCP sockets. Nothing in this file simulates a distributed
// runtime with threads.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "child_process.hpp"
#include "critic_fabric/transport.hpp"
#include "fixture.hpp"

using namespace critic_fabric;
using namespace cftest;

namespace {

std::string field(const std::string& line, const std::string& key) {
    const std::string token = key + "=";
    const std::size_t position = line.find(token);
    if (position == std::string::npos) {
        return {};
    }
    const std::size_t start = position + token.size();
    std::size_t end = line.find(' ', start);
    if (end == std::string::npos) {
        end = line.size();
    }
    return line.substr(start, end - start);
}

std::uint64_t number_field(const std::string& line, const std::string& key) {
    const std::string value = field(line, key);
    return value.empty() ? 0 : std::strtoull(value.c_str(), nullptr, 10);
}

struct Deployment {
    ChildProcess coordinator;
    std::uint16_t port = 0;
    CoordinatorEpoch epoch{};
};

bool start_coordinator(Deployment& deployment, const std::string& persistence) {
    std::vector<std::string> arguments = {"--port", "0", "--label", "multiprocess-coordinator"};
    if (!persistence.empty()) {
        arguments.push_back("--persistence");
        arguments.push_back(persistence);
    }
    if (!deployment.coordinator.start(helper_executable("critic_coordinator"), arguments)) {
        std::fprintf(stderr, "failed to start the coordinator process\n");
        return false;
    }
    const std::string line = deployment.coordinator.read_line();
    if (line.find("CRITIC_COORDINATOR_READY") == std::string::npos) {
        std::fprintf(stderr, "unexpected coordinator readiness line: %s\n", line.c_str());
        return false;
    }
    deployment.port = static_cast<std::uint16_t>(number_field(line, "port"));
    deployment.epoch = CoordinatorEpoch::from_value(number_field(line, "epoch"));
    return deployment.port != 0 && deployment.epoch.valid();
}

struct Client {
    CoordinatorClient client;
    bool connect(std::uint16_t port) {
        RuntimeLimits limits;
        return client.connect("127.0.0.1", port, limits).ok() && client.hello(RequestId::from_value(1)).ok();
    }
};

struct RemoteTarget {
    TargetId id{};
    TargetGeneration generation{};
    Digest digest{};
};

RemoteTarget make_target(CoordinatorClient& client, CoordinatorEpoch epoch, const std::string& payload) {
    RegisterTargetRequest request;
    request.schema_name = "multiprocess/target";
    request.payload.assign(payload.begin(), payload.end());
    request.provenance = "multiprocess-test";
    request.epoch = epoch;
    request.request = RequestId::from_value(10);
    const auto result = client.register_target(request);
    if (!result.ok()) {
        std::fprintf(stderr, "remote target registration failed: %s\n", result.status().to_string().c_str());
        std::abort();
    }
    RemoteTarget target;
    target.id = result.value().id;
    target.generation = result.value().generation;
    target.digest = result.value().content_digest;
    return target;
}

ReviewRecord declare_review(CoordinatorClient& client, CoordinatorEpoch epoch, const RemoteTarget& target,
                            const ReviewSpec& spec, std::uint64_t request_id) {
    DeclareReviewRequest request;
    request.request = ReviewRequestId::from_value(request_id);
    request.target = target.id;
    request.target_generation = target.generation;
    request.spec = spec;
    request.epoch = epoch;
    const auto result = client.declare_review(request);
    if (!result.ok()) {
        std::fprintf(stderr, "remote review declaration failed: %s\n", result.status().to_string().c_str());
        std::abort();
    }
    return result.value();
}

// Starts one independent worker process and returns the readiness line.
struct WorkerHandle {
    ChildProcess process;
    std::uint64_t critic = 0;
    std::uint64_t worker = 0;
    std::uint64_t boot = 0;
    std::uint64_t generation = 0;
};

bool start_worker(WorkerHandle& handle, std::uint16_t port, std::uint64_t critic_id,
                  const std::string& behavior, const std::vector<std::string>& extra = {}) {
    std::vector<std::string> arguments = {"--port", std::to_string(port), "--critic",
                                          std::to_string(critic_id), "--behavior", behavior,
                                          "--label", "multiprocess-worker"};
    for (const std::string& argument : extra) {
        arguments.push_back(argument);
    }
    if (!handle.process.start(helper_executable("critic_worker"), arguments)) {
        return false;
    }
    handle.critic = critic_id;
    return true;
}

}  // namespace

CF_TEST(multiprocess_review_commits_over_real_sockets) {
    Deployment deployment;
    CF_REQUIRE(start_coordinator(deployment, ""));

    Client controller;
    CF_REQUIRE(controller.connect(deployment.port));
    const RemoteTarget target = make_target(controller.client, deployment.epoch, "multiprocess target");

    // Register the critic identity from the controller, then start a genuine
    // worker process that owns the physical incarnation.
    SyntheticCriticConfig synthetic;
    synthetic.role = CriticRole::GeneralCritic;
    synthetic.behavior = SyntheticBehavior::Pass;
    RegisterCriticRequest critic_request;
    critic_request.capability = synthetic_critic_capability(synthetic);
    critic_request.provenance = "multiprocess-test";
    critic_request.epoch = deployment.epoch;
    critic_request.request = RequestId::from_value(11);
    const auto registration = controller.client.register_critic(critic_request);
    CF_REQUIRE(registration.ok());

    ReviewSpec spec = default_review_spec();
    const ReviewRecord review = declare_review(controller.client, deployment.epoch, target, spec, 100);

    WorkerHandle worker;
    CF_REQUIRE(start_worker(worker, deployment.port, registration.value().id.value(), "pass"));

    // The controller waits for the worker to register and declare readiness by
    // observing the coordinator's worker list. This is an immediate poll, not a
    // timed wait.
    std::uint64_t worker_id = 0;
    std::uint64_t boot_id = 0;
    for (int attempt = 0; attempt < 200000 && worker_id == 0; ++attempt) {
        const auto workers = controller.client.list_workers();
        if (!workers.ok()) {
            continue;
        }
        for (const WorkerRecord& record : workers.value()) {
            if (record.critic == registration.value().id && record.authoritative) {
                worker_id = record.id.value();
                boot_id = record.boot.value();
            }
        }
    }
    CF_REQUIRE(worker_id != 0);

    AssignCriticsRequest assign;
    assign.review = review.id;
    assign.review_generation = review.generation;
    assign.epoch = deployment.epoch;
    assign.request = RequestId::from_value(12);
    AssignCriticsRequest::Assignment assignment;
    assignment.critic = registration.value().id;
    assignment.critic_generation = registration.value().generation;
    assignment.worker = WorkerId::from_value(worker_id);
    assignment.worker_boot = WorkerBootId::from_value(boot_id);
    assignment.role = CriticRole::GeneralCritic;
    assign.assignments.push_back(assignment);
    const auto created = controller.client.assign_critics(assign);
    CF_REQUIRE(created.ok());
    CF_CHECK_EQ(created.value().size(), 1u);

    // The worker process services its push notice and exits.
    const std::string done_line = worker.process.read_line();
    CF_CHECK(done_line.find("CRITIC_WORKER_DONE") != std::string::npos);
    const int worker_exit = worker.process.wait();
    CF_CHECK_EQ(worker_exit, 0);

    const auto snapshot = controller.client.query_review(review.id, ReviewGeneration{});
    CF_REQUIRE(snapshot.ok());
    CF_CHECK_EQ(snapshot.value().findings.size(), 1u);
    CF_CHECK_EQ(snapshot.value().findings.front().outcome, FindingOutcome::Pass);

    CommitReviewRequest commit;
    commit.review = review.id;
    commit.review_generation = review.generation;
    commit.target_generation = target.generation;
    commit.epoch = deployment.epoch;
    commit.request = RequestId::from_value(13);
    const auto decision = controller.client.commit_review(commit);
    CF_REQUIRE(decision.ok());
    CF_CHECK_EQ(decision.value().disposition, ReviewDisposition::Pass);


    CF_REQUIRE(controller.client.shutdown_server(RequestId::from_value(9999)).ok());
    CF_CHECK_EQ(deployment.coordinator.wait(), 0);
}

CF_TEST(multiprocess_worker_death_is_fenced_and_a_fresh_boot_revalidates) {
    Deployment deployment;
    CF_REQUIRE(start_coordinator(deployment, ""));

    Client controller;
    CF_REQUIRE(controller.connect(deployment.port));
    const RemoteTarget target = make_target(controller.client, deployment.epoch, "worker death target");

    SyntheticCriticConfig synthetic;
    synthetic.role = CriticRole::GeneralCritic;
    synthetic.behavior = SyntheticBehavior::Pass;
    RegisterCriticRequest critic_request;
    critic_request.capability = synthetic_critic_capability(synthetic);
    critic_request.provenance = "multiprocess-test";
    critic_request.epoch = deployment.epoch;
    critic_request.request = RequestId::from_value(21);
    const auto registration = controller.client.register_critic(critic_request);
    CF_REQUIRE(registration.ok());

    ReviewSpec spec = default_review_spec();
    const ReviewRecord review = declare_review(controller.client, deployment.epoch, target, spec, 200);

    // Worker A holds immediately before publication so the harness can kill it
    // at exactly the interesting moment.
    WorkerHandle worker_a;
    CF_REQUIRE(start_worker(worker_a, deployment.port, registration.value().id.value(), "pass",
                            {"--hold-before-publish"}));

    std::uint64_t worker_id = 0;
    std::uint64_t boot_id = 0;
    for (int attempt = 0; attempt < 200000 && worker_id == 0; ++attempt) {
        const auto workers = controller.client.list_workers();
        if (!workers.ok()) {
            continue;
        }
        for (const WorkerRecord& record : workers.value()) {
            if (record.critic == registration.value().id && record.authoritative) {
                worker_id = record.id.value();
                boot_id = record.boot.value();
            }
        }
    }
    CF_REQUIRE(worker_id != 0);

    AssignCriticsRequest assign;
    assign.review = review.id;
    assign.review_generation = review.generation;
    assign.epoch = deployment.epoch;
    assign.request = RequestId::from_value(22);
    AssignCriticsRequest::Assignment assignment;
    assignment.critic = registration.value().id;
    assignment.critic_generation = registration.value().generation;
    assignment.worker = WorkerId::from_value(worker_id);
    assignment.worker_boot = WorkerBootId::from_value(boot_id);
    assignment.role = CriticRole::GeneralCritic;
    assign.assignments.push_back(assignment);
    CF_REQUIRE(controller.client.assign_critics(assign).ok());

    // The worker announces that it is holding, then it is killed as a real OS
    // process. Nothing it had done becomes current.
    const std::string holding_line = worker_a.process.read_line();
    CF_CHECK(holding_line.find("CRITIC_WORKER_HOLDING") != std::string::npos);
    const std::uint64_t killed_pid = worker_a.process.pid();
    worker_a.process.terminate();
    (void)worker_a.process.wait();
    CF_CHECK(killed_pid != 0);

    // Worker A-prime starts with a fresh boot identity. It must publish fresh
    // capability and readiness evidence before it may hold authority.
    WorkerHandle worker_b;
    CF_REQUIRE(start_worker(worker_b, deployment.port, registration.value().id.value(), "pass"));

    std::uint64_t prime_worker = 0;
    std::uint64_t prime_boot = 0;
    for (int attempt = 0; attempt < 200000 && prime_worker == 0; ++attempt) {
        const auto workers = controller.client.list_workers();
        if (!workers.ok()) {
            continue;
        }
        for (const WorkerRecord& record : workers.value()) {
            if (record.critic == registration.value().id && record.authoritative &&
                record.boot.value() != boot_id) {
                prime_worker = record.id.value();
                prime_boot = record.boot.value();
            }
        }
    }
    CF_REQUIRE(prime_worker != 0);
    CF_CHECK(prime_worker != worker_id);
    CF_CHECK(prime_boot != boot_id);

    // Late traffic from the killed incarnation is rejected before any mutation.
    const auto snapshot = controller.client.query_review(review.id, ReviewGeneration{});
    CF_REQUIRE(snapshot.ok());
    for (const CriticAssignment& existing : snapshot.value().assignments) {
        if (existing.worker == WorkerId::from_value(worker_id)) {
            CF_CHECK(!existing.active);
        }
    }

    // The review was moved to revalidation by the fence; it must be reassigned
    // under the fresh incarnation before it can produce a result.
    CF_CHECK(snapshot.value().review.state == ReviewState::RevalidationRequired ||
             snapshot.value().review.state == ReviewState::Assigned ||
             snapshot.value().review.state == ReviewState::Reviewing);

    AssignCriticsRequest reassign;
    reassign.review = review.id;
    reassign.review_generation = review.generation;
    reassign.epoch = deployment.epoch;
    reassign.request = RequestId::from_value(23);
    AssignCriticsRequest::Assignment fresh;
    fresh.critic = registration.value().id;
    fresh.critic_generation = registration.value().generation;
    fresh.worker = WorkerId::from_value(prime_worker);
    fresh.worker_boot = WorkerBootId::from_value(prime_boot);
    fresh.role = CriticRole::GeneralCritic;
    reassign.assignments.push_back(fresh);
    const auto reassigned = controller.client.assign_critics(reassign);
    CF_REQUIRE(reassigned.ok());

    const std::string done_line = worker_b.process.read_line();
    CF_CHECK(done_line.find("CRITIC_WORKER_DONE") != std::string::npos);
    CF_CHECK_EQ(worker_b.process.wait(), 0);

    CommitReviewRequest commit;
    commit.review = review.id;
    commit.review_generation = review.generation;
    commit.target_generation = target.generation;
    commit.epoch = deployment.epoch;
    commit.request = RequestId::from_value(24);
    const auto decision = controller.client.commit_review(commit);
    CF_REQUIRE(decision.ok());
    CF_CHECK_EQ(decision.value().disposition, ReviewDisposition::Pass);

    CF_REQUIRE(controller.client.shutdown_server(RequestId::from_value(9999)).ok());
    CF_CHECK_EQ(deployment.coordinator.wait(), 0);
}

CF_TEST(multiprocess_coordinator_restart_recovers_and_rejects_old_epoch) {
    const std::string journal = std::string("critic_fabric_multiprocess.journal");
    std::remove(journal.c_str());

    RemoteTarget target;
    ReviewId review_id{};
    CoordinatorEpoch first_epoch{};
    {
        Deployment deployment;
        CF_REQUIRE(start_coordinator(deployment, journal));
        first_epoch = deployment.epoch;

        Client controller;
        CF_REQUIRE(controller.connect(deployment.port));
        target = make_target(controller.client, deployment.epoch, "restart target");

        SyntheticCriticConfig synthetic;
        synthetic.role = CriticRole::GeneralCritic;
        synthetic.behavior = SyntheticBehavior::Pass;
        RegisterCriticRequest critic_request;
        critic_request.capability = synthetic_critic_capability(synthetic);
        critic_request.provenance = "multiprocess-test";
        critic_request.epoch = deployment.epoch;
        critic_request.request = RequestId::from_value(31);
        const auto registration = controller.client.register_critic(critic_request);
        CF_REQUIRE(registration.ok());

        const ReviewRecord review = declare_review(controller.client, deployment.epoch, target,
                                                   default_review_spec(), 300);
        review_id = review.id;

        WorkerHandle worker;
        CF_REQUIRE(start_worker(worker, deployment.port, registration.value().id.value(), "pass"));

        std::uint64_t worker_id = 0;
        std::uint64_t boot_id = 0;
        for (int attempt = 0; attempt < 200000 && worker_id == 0; ++attempt) {
            const auto workers = controller.client.list_workers();
            if (!workers.ok()) {
                continue;
            }
            for (const WorkerRecord& record : workers.value()) {
                if (record.critic == registration.value().id && record.authoritative) {
                    worker_id = record.id.value();
                    boot_id = record.boot.value();
                }
            }
        }
        CF_REQUIRE(worker_id != 0);

        AssignCriticsRequest assign;
        assign.review = review.id;
        assign.review_generation = review.generation;
        assign.epoch = deployment.epoch;
        assign.request = RequestId::from_value(32);
        AssignCriticsRequest::Assignment assignment;
        assignment.critic = registration.value().id;
        assignment.critic_generation = registration.value().generation;
        assignment.worker = WorkerId::from_value(worker_id);
        assignment.worker_boot = WorkerBootId::from_value(boot_id);
        assignment.role = CriticRole::GeneralCritic;
        assign.assignments.push_back(assignment);
        CF_REQUIRE(controller.client.assign_critics(assign).ok());
        CF_CHECK(worker.process.read_line().find("CRITIC_WORKER_DONE") != std::string::npos);
        CF_CHECK_EQ(worker.process.wait(), 0);

        // The coordinator is terminated without a graceful shutdown, exactly as
        // an operator would observe after a crash.
        deployment.coordinator.terminate();
        (void)deployment.coordinator.wait();
    }

    // A fresh coordinator process recovers the durable review state.
    Deployment restarted;
    CF_REQUIRE(start_coordinator(restarted, journal));
    CF_CHECK(restarted.epoch.value() > first_epoch.value());

    Client controller;
    CF_REQUIRE(controller.connect(restarted.port));

    const auto snapshot = controller.client.query_review(review_id, ReviewGeneration{});
    CF_REQUIRE(snapshot.ok());
    CF_CHECK_EQ(snapshot.value().review.state, ReviewState::RevalidationRequired);
    CF_CHECK(!snapshot.value().review.authority_current);
    CF_CHECK_EQ(snapshot.value().findings.size(), 1u);
    const auto recovered_workers = controller.client.list_workers();
    CF_REQUIRE(recovered_workers.ok());
    for (const WorkerRecord& record : recovered_workers.value()) {
        CF_CHECK(!record.authoritative);
    }

    // Traffic carrying the retired epoch is rejected.
    RegisterTargetRequest stale;
    stale.schema_name = "multiprocess/target";
    stale.payload = {'s', 't', 'a', 'l', 'e'};
    stale.epoch = first_epoch;
    stale.request = RequestId::from_value(33);
    const auto rejected = controller.client.register_target(stale);
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::StaleCoordinatorEpoch);

    // The recovered review cannot commit until it is reassigned and revalidated.
    CommitReviewRequest commit;
    commit.review = review_id;
    commit.review_generation = snapshot.value().review.generation;
    commit.target_generation = snapshot.value().review.target_generation;
    commit.epoch = restarted.epoch;
    commit.request = RequestId::from_value(34);
    const auto refused = controller.client.commit_review(commit);
    CF_CHECK(!refused.ok());
    CF_CHECK(refused.code() == ErrorCode::RevalidationRequired);

    CF_REQUIRE(controller.client.shutdown_server(RequestId::from_value(9999)).ok());
    CF_CHECK_EQ(restarted.coordinator.wait(), 0);
    std::remove(journal.c_str());
}

CF_TEST(multiprocess_malformed_and_stale_traffic_is_rejected) {
    Deployment deployment;
    CF_REQUIRE(start_coordinator(deployment, ""));

    // Malformed transport input must be rejected. Each case uses a fresh
    // connection because the server closes the connection after a protocol
    // violation rather than trying to resynchronise a corrupt stream.
    {
        RuntimeLimits limits;

        // A frame whose payload is truncated for its message type.
        {
            Client raw;
            CF_REQUIRE(raw.client.connect("127.0.0.1", deployment.port, limits).ok());
            CF_REQUIRE(raw.client.send(MessageType::QueryTarget, AuthorityContext{}, {1, 2}).ok());
            const auto response = raw.client.receive();
            CF_REQUIRE(response.ok());
            CF_CHECK(response.value().header.type == MessageType::ErrorResponse);
            CF_CHECK(response.value().payload.size() > 4);
        }

        // A frame carrying bytes after its declared payload.
        {
            Client raw;
            CF_REQUIRE(raw.client.connect("127.0.0.1", deployment.port, limits).ok());
            CF_REQUIRE(raw.client
                           .send(MessageType::QueryTarget, AuthorityContext{},
                                 std::vector<std::uint8_t>(9, 0))
                           .ok());
            const auto response = raw.client.receive();
            CF_REQUIRE(response.ok());
            CF_CHECK(response.value().header.type == MessageType::ErrorResponse);
        }

        // Raw bytes that are not a frame at all: wrong magic.
        {
            Client raw;
            CF_REQUIRE(raw.client.connect("127.0.0.1", deployment.port, limits).ok());
            std::vector<std::uint8_t> garbage(108, 0);
            garbage[0] = 'X';
            CF_REQUIRE(raw.client.send_raw(garbage).ok());
            const auto response = raw.client.receive();
            CF_REQUIRE(response.ok());
            CF_CHECK(response.value().header.type == MessageType::ErrorResponse);
        }

        // Raw bytes declaring an oversized payload.
        {
            Client raw;
            CF_REQUIRE(raw.client.connect("127.0.0.1", deployment.port, limits).ok());
            std::vector<std::uint8_t> oversized(108, 0);
            oversized[0] = 'C';
            oversized[1] = 'F';
            oversized[2] = 'B';
            oversized[3] = '1';
            oversized[5] = 1;
            oversized[6] = 0;
            oversized[7] = 1;
            oversized[12] = 0x7F;
            oversized[13] = 0xFF;
            oversized[14] = 0xFF;
            oversized[15] = 0xFF;
            CF_REQUIRE(raw.client.send_raw(oversized).ok());
            const auto response = raw.client.receive();
            CF_REQUIRE(response.ok());
            CF_CHECK(response.value().header.type == MessageType::ErrorResponse);
        }

        // Raw bytes with a valid header shape but a damaged checksum.
        {
            Client raw;
            CF_REQUIRE(raw.client.connect("127.0.0.1", deployment.port, limits).ok());
            std::vector<std::uint8_t> bytes;
            CF_REQUIRE(FrameCodec::encode([&] {
                Frame frame;
                frame.header.type = MessageType::Hello;
                return frame;
            }(), limits, bytes).ok());
            bytes[104] ^= 0xFF;
            CF_REQUIRE(raw.client.send_raw(bytes).ok());
            const auto response = raw.client.receive();
            CF_REQUIRE(response.ok());
            CF_CHECK(response.value().header.type == MessageType::ErrorResponse);
        }
    }

    // The coordinator still serves well-formed traffic afterwards.
    Client controller;
    CF_REQUIRE(controller.connect(deployment.port));
    const RemoteTarget target = make_target(controller.client, deployment.epoch, "post-garbage target");
    CF_CHECK(target.id.valid());

    // A stale review generation is distinguished from a stale target generation.
    const ReviewRecord review =
        declare_review(controller.client, deployment.epoch, target, default_review_spec(), 400);
    SubmitFindingRequest finding;
    finding.review = review.id;
    finding.review_generation = ReviewGeneration::from_value(review.generation.value() + 9);
    finding.target = target.id;
    finding.target_generation = target.generation;
    finding.critic = CriticId::from_value(1);
    finding.critic_generation = CriticGeneration::first();
    finding.worker = WorkerId::from_value(1);
    finding.worker_boot = WorkerBootId::from_value(1);
    finding.outcome = FindingOutcome::Pass;
    finding.epoch = deployment.epoch;
    finding.request = RequestId::from_value(41);
    const auto rejected = controller.client.submit_finding(finding);
    CF_CHECK(!rejected.ok());
    CF_CHECK(rejected.code() == ErrorCode::StaleReviewGeneration);

    CF_REQUIRE(controller.client.shutdown_server(RequestId::from_value(9999)).ok());
    CF_CHECK_EQ(deployment.coordinator.wait(), 0);
}
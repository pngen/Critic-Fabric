// Critic Fabric — loopback TCP transport for the distributed reference topology.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_TRANSPORT_HPP
#define CRITIC_FABRIC_TRANSPORT_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "critic_fabric/coordinator.hpp"
#include "critic_fabric/limits.hpp"
#include "critic_fabric/protocol.hpp"
#include "critic_fabric/result.hpp"

namespace critic_fabric {

// Initialises and tears down the platform socket layer exactly once per process.
class SocketRuntime {
public:
    static Status ensure_initialised();
    static void shutdown();
};

struct ServerConfig {
    std::string bind_address = "127.0.0.1";
    std::uint16_t port = 0;  // zero requests an ephemeral port
    RuntimeLimits limits;
    bool fence_workers_on_disconnect = true;
    // Invoked after the acknowledgement for an administrative shutdown request
    // has been written. It is called with no lock held, so the callback may
    // safely stop the server.
    std::function<void()> shutdown_requested;
};

// Multi-threaded loopback server. One accept thread and one thread per
// connection, bounded by the configured worker connection limit.
//
// Lock ownership: the connection registry mutex is never held across socket
// I/O; a connection lookup yields a shared pointer that is used only after the
// registry lock is released.
class CoordinatorServer {
public:
    CoordinatorServer(Coordinator& coordinator, ServerConfig config);
    ~CoordinatorServer();

    CoordinatorServer(const CoordinatorServer&) = delete;
    CoordinatorServer& operator=(const CoordinatorServer&) = delete;

    Status start();
    Status stop();

    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
    [[nodiscard]] bool running() const noexcept { return running_.load(); }

 private:
    struct Connection;
    using ConnectionPtr = std::shared_ptr<Connection>;

    void accept_loop();
    void serve(const ConnectionPtr& connection);
    Status dispatch(const ConnectionPtr& connection, const Frame& request, Frame& response);
    Status send_frame(const ConnectionPtr& connection, const Frame& frame);
    void register_worker(const ConnectionPtr& connection, WorkerId worker);
    void unregister_worker(WorkerId worker);
    ConnectionPtr find_worker_connection(WorkerId worker);
    void forget_connection(const ConnectionPtr& connection, bool fence_workers);
    void notify_assignments(ReviewId review, ReviewGeneration generation);

    Coordinator& coordinator_;
    ServerConfig config_;
    mutable std::mutex registry_mutex_;
    std::map<WorkerId, std::weak_ptr<Connection>> workers_;
    std::vector<ConnectionPtr> connections_;
    std::thread accept_thread_;
    std::vector<std::pair<std::thread, ConnectionPtr>> workers_threads_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::uintptr_t listener_ = static_cast<std::uintptr_t>(-1);
    std::uint16_t bound_port_ = 0;
};

// Blocking client. A single outstanding request is serialised by an internal
// mutex; asynchronous server pushes are surfaced through the push callback.
class CoordinatorClient {
public:
    using PushHandler = std::function<void(const Frame&)>;

    CoordinatorClient() = default;
    ~CoordinatorClient();

    CoordinatorClient(const CoordinatorClient&) = delete;
    CoordinatorClient& operator=(const CoordinatorClient&) = delete;

    Status connect(const std::string& host, std::uint16_t port, const RuntimeLimits& limits);
    void close();
    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
    [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] const RuntimeLimits& limits() const noexcept { return limits_; }

    void set_push_handler(PushHandler handler);

    Status send(MessageType type, const AuthorityContext& authority, const std::vector<std::uint8_t>& payload);
    // Writes raw bytes that are not necessarily a well-formed frame. Used to
    // prove that malformed transport input is rejected rather than tolerated.
    Status send_raw(const std::vector<std::uint8_t>& bytes);
    Result<Frame> receive();
    Result<Frame> transact(MessageType type, const AuthorityContext& authority,
                           const std::vector<std::uint8_t>& payload, MessageType expected_ack);

    // Performs the handshake and records the coordinator epoch.
    Status hello(RequestId correlation);

    // Typed convenience wrappers. Each returns the decoded body or the typed
    // rejection the coordinator produced.
    Result<TargetRecord> register_target(const RegisterTargetRequest& request);
    Result<TargetRecord> supersede_target(const SupersedeTargetRequest& request);
    Result<CriticRegistration> register_critic(const RegisterCriticRequest& request);
    Result<WorkerRecord> register_worker(const RegisterWorkerRequest& request);
    Result<WorkerRecord> declare_readiness(const DeclareReadinessRequest& request);
    Result<ReviewRecord> declare_review(const DeclareReviewRequest& request);
    Result<std::vector<CriticAssignment>> assign_critics(const AssignCriticsRequest& request);
    Result<EvidenceRecord> submit_evidence(const SubmitEvidenceRequest& request);
    Result<Finding> submit_finding(const SubmitFindingRequest& request);
    Result<Challenge> submit_challenge(const SubmitChallengeRequest& request);
    Result<Rebuttal> submit_rebuttal(const SubmitRebuttalRequest& request);
    Result<Decision> commit_review(const CommitReviewRequest& request);
    Result<ReviewRecord> cancel_review(const CancelReviewRequest& request);
    Result<ReviewSnapshot> query_review(ReviewId review, ReviewGeneration generation);
    Result<Decision> query_decision(ReviewId review, ReviewGeneration generation);
    Result<std::vector<ReviewSummary>> list_reviews();
    Result<std::vector<WorkerRecord>> list_workers();
    Result<TargetRecord> query_target(TargetId target);
    Result<std::vector<CriticAssignment>> query_assignments(WorkerId worker);
    Status fence_worker(WorkerId worker, const std::string& reason, RequestId correlation);
    Status shutdown_server(RequestId correlation);

private:
    Result<Frame> exchange(MessageType type, const AuthorityContext& authority,
                           const std::vector<std::uint8_t>& payload, MessageType expected_ack);

    std::uintptr_t socket_ = static_cast<std::uintptr_t>(-1);
    std::atomic<bool> connected_{false};
    mutable std::mutex request_mutex_;
    PushHandler push_handler_;
    RuntimeLimits limits_;
    CoordinatorEpoch epoch_{};
};

// Decodes a status envelope followed by a typed body. Trailing bytes inside a
// response payload are rejected rather than ignored.
template <typename T>
Result<T> decode_typed_response(const Frame& frame, const RuntimeLimits& limits) {
    ReaderPolicy policy;
    policy.truncated = ErrorCode::ProtocolTruncated;
    policy.oversized = ErrorCode::ProtocolOversized;
    policy.invalid_enum = ErrorCode::ProtocolInvalidEnum;
    policy.malformed = ErrorCode::ProtocolError;
    ByteReader reader(frame.payload, policy);
    StatusEnvelope envelope;
    Status status = decode(reader, envelope, limits);
    if (!status.ok()) {
        return status;
    }
    if (envelope.code != ErrorCode::Ok) {
        return Status::error(envelope.code, envelope.detail);
    }
    T value{};
    status = decode(reader, limits, value);
    if (!status.ok()) {
        return status;
    }
    if (!reader.at_end()) {
        return Status::error(ErrorCode::ProtocolTrailingGarbage,
                             "response payload carries bytes after the declared body");
    }
    return value;
}

// Decodes a status envelope followed by a length-prefixed list of typed items.
template <typename T>
Result<std::vector<T>> decode_list_response(const Frame& frame, const RuntimeLimits& limits,
                                            std::uint32_t max_items) {
    ReaderPolicy policy;
    policy.truncated = ErrorCode::ProtocolTruncated;
    policy.oversized = ErrorCode::ProtocolOversized;
    policy.invalid_enum = ErrorCode::ProtocolInvalidEnum;
    policy.malformed = ErrorCode::ProtocolError;
    ByteReader reader(frame.payload, policy);
    StatusEnvelope envelope;
    Status status = decode(reader, envelope, limits);
    if (!status.ok()) {
        return status;
    }
    if (envelope.code != ErrorCode::Ok) {
        return Status::error(envelope.code, envelope.detail);
    }
    const std::uint32_t count = reader.u32();
    if (!reader.ok()) {
        return reader.status();
    }
    if (count > max_items) {
        return Status::error(ErrorCode::ProtocolOversized, "list response exceeds the configured bound");
    }
    std::vector<T> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        T item{};
        status = decode(reader, limits, item);
        if (!status.ok()) {
            return status;
        }
        out.push_back(std::move(item));
    }
    if (!reader.at_end()) {
        return Status::error(ErrorCode::ProtocolTrailingGarbage,
                             "list response carries bytes after the declared body");
    }
    return out;
}

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_TRANSPORT_HPP

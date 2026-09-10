// Critic Fabric — loopback TCP transport implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/transport.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace critic_fabric {
namespace {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr native_socket kInvalidNative = INVALID_SOCKET;
#else
using native_socket = int;
constexpr native_socket kInvalidNative = -1;
#endif

native_socket to_native(std::uintptr_t value) { return static_cast<native_socket>(value); }

std::uintptr_t from_native(native_socket value) { return static_cast<std::uintptr_t>(value); }

void close_native(native_socket socket) {
    if (socket == kInvalidNative) {
        return;
    }
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
    ::closesocket(socket);
#else
    ::shutdown(socket, SHUT_RDWR);
    ::close(socket);
#endif
}

void interrupt_native(native_socket socket) {
    if (socket == kInvalidNative) {
        return;
    }
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

int last_socket_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool would_block(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

void set_no_delay(native_socket socket) {
    int one = 1;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

Status read_exact(native_socket socket, std::uint8_t* buffer, std::size_t length) {
    std::size_t received = 0;
    while (received < length) {
        const int chunk = static_cast<int>((std::min)(length - received, static_cast<std::size_t>(1u << 20)));
        const int got = ::recv(socket, reinterpret_cast<char*>(buffer + received), chunk, 0);
        if (got == 0) {
            return Status::error(ErrorCode::ProtocolConnectionClosed,
                                 "peer closed the connection before the frame was complete");
        }
        if (got < 0) {
            const int error = last_socket_error();
            if (would_block(error)) {
                continue;
            }
            return Status::error(ErrorCode::ProtocolConnectionClosed, "connection read failed");
        }
        received += static_cast<std::size_t>(got);
    }
    return Status::success();
}

Status write_all(native_socket socket, const std::uint8_t* buffer, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const int chunk = static_cast<int>((std::min)(length - sent, static_cast<std::size_t>(1u << 20)));
        const int wrote = ::send(socket, reinterpret_cast<const char*>(buffer + sent), chunk, 0);
        if (wrote <= 0) {
            const int error = last_socket_error();
            if (wrote < 0 && would_block(error)) {
                continue;
            }
            return Status::error(ErrorCode::ProtocolConnectionClosed, "connection write failed");
        }
        sent += static_cast<std::size_t>(wrote);
    }
    return Status::success();
}

ReaderPolicy payload_policy() {
    ReaderPolicy policy;
    policy.truncated = ErrorCode::ProtocolTruncated;
    policy.oversized = ErrorCode::ProtocolOversized;
    policy.invalid_enum = ErrorCode::ProtocolInvalidEnum;
    policy.malformed = ErrorCode::ProtocolError;
    return policy;
}

}  // namespace

// --- SocketRuntime -----------------------------------------------------------

namespace {
std::mutex g_socket_mutex;
int g_socket_users = 0;
bool g_socket_ready = false;
}  // namespace

Status SocketRuntime::ensure_initialised() {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_socket_ready) {
        g_socket_users += 1;
        return Status::success();
    }
#ifdef _WIN32
    WSADATA data;
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return Status::error(ErrorCode::InternalError, "Winsock initialisation failed");
    }
#endif
    g_socket_ready = true;
    g_socket_users += 1;
    return Status::success();
}

void SocketRuntime::shutdown() {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (!g_socket_ready) {
        return;
    }
    g_socket_users -= 1;
    if (g_socket_users <= 0) {
        g_socket_users = 0;
#ifdef _WIN32
        ::WSACleanup();
#endif
        g_socket_ready = false;
    }
}

// --- CoordinatorServer -------------------------------------------------------

struct CoordinatorServer::Connection {
    native_socket socket = kInvalidNative;
    std::mutex write_mutex;
    std::atomic<bool> closed{false};
    std::vector<WorkerId> workers;
};

CoordinatorServer::CoordinatorServer(Coordinator& coordinator, ServerConfig config)
    : coordinator_(coordinator), config_(std::move(config)) {
    config_.limits.clamp_to_hard_ceiling();
}

CoordinatorServer::~CoordinatorServer() { stop(); }

Status CoordinatorServer::start() {
    if (running_.load()) {
        return Status::error(ErrorCode::InternalError, "server is already running");
    }
    Status status = SocketRuntime::ensure_initialised();
    if (!status.ok()) {
        return status;
    }

    native_socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidNative) {
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::InternalError, "unable to create a listening socket");
    }
    const int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        close_native(listener);
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::InternalError, "bind address is not a valid IPv4 literal");
    }
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_native(listener);
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::InternalError, "unable to bind the listening socket");
    }
    if (::listen(listener, 64) != 0) {
        close_native(listener);
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::InternalError, "unable to listen on the bound socket");
    }

    sockaddr_in bound;
    std::memset(&bound, 0, sizeof(bound));
#ifdef _WIN32
    int bound_length = static_cast<int>(sizeof(bound));
#else
    socklen_t bound_length = sizeof(bound);
#endif
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
        close_native(listener);
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::InternalError, "unable to read the bound socket address");
    }
    bound_port_ = ntohs(bound.sin_port);
    listener_ = from_native(listener);
    stopping_.store(false);
    running_.store(true);
    accept_thread_ = std::thread([this] { accept_loop(); });
    return Status::success();
}

Status CoordinatorServer::stop() {
    if (!running_.load() && listener_ == static_cast<std::uintptr_t>(-1)) {
        return Status::success();
    }
    stopping_.store(true);
    const native_socket listener = to_native(listener_);
    close_native(listener);
    listener_ = static_cast<std::uintptr_t>(-1);
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    std::vector<ConnectionPtr> to_close;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        to_close = connections_;
    }
    for (const ConnectionPtr& connection : to_close) {
        connection->closed.store(true);
        interrupt_native(connection->socket);
    }
    for (auto& entry : workers_threads_) {
        if (entry.first.joinable()) {
            entry.first.join();
        }
    }
    workers_threads_.clear();
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        for (const ConnectionPtr& connection : connections_) {
            close_native(connection->socket);
        }
        connections_.clear();
        workers_.clear();
    }
    running_.store(false);
    SocketRuntime::shutdown();
    return Status::success();
}

void CoordinatorServer::accept_loop() {
    const native_socket listener = to_native(listener_);
    while (!stopping_.load()) {
        // Readiness check rather than an unbounded blocking accept, so the
        // server can be stopped deterministically without relying on the
        // platform's undefined behaviour when a listening socket is closed from
        // another thread. The interval is a readiness poll, not a wait for work
        // to finish.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval interval;
        interval.tv_sec = 0;
        interval.tv_usec = 1000;
        const int ready = ::select(0, &readable, nullptr, nullptr, &interval);
        if (stopping_.load()) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        sockaddr_in peer;
        std::memset(&peer, 0, sizeof(peer));
#ifdef _WIN32
        int peer_length = static_cast<int>(sizeof(peer));
#else
        socklen_t peer_length = sizeof(peer);
#endif
        const native_socket accepted =
            ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (accepted == kInvalidNative) {
            if (stopping_.load()) {
                break;
            }
            continue;
        }
        set_no_delay(accepted);

        ConnectionPtr connection = std::make_shared<Connection>();
        connection->socket = accepted;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            if (connections_.size() >= config_.limits.max_worker_connections) {
                close_native(accepted);
                continue;
            }
            connections_.push_back(connection);
            // Reap finished connection threads so the per-connection thread count
            // stays bounded by the configured limit.
            for (auto it = workers_threads_.begin(); it != workers_threads_.end();) {
                if (it->second->closed.load()) {
                    if (it->first.joinable()) {
                        it->first.join();
                    }
                    it = workers_threads_.erase(it);
                } else {
                    ++it;
                }
            }
            workers_threads_.emplace_back([this, connection] { serve(connection); }, connection);
        }
    }
}

void CoordinatorServer::serve(const ConnectionPtr& connection) {
    const native_socket socket = connection->socket;
    while (!stopping_.load() && !connection->closed.load()) {
        std::uint8_t header_bytes[FrameCodec::kHeaderBytes];
        Status status = read_exact(socket, header_bytes, FrameCodec::kHeaderBytes);
        if (!status.ok()) {
            break;
        }
        FrameHeader header;
        status = FrameCodec::decode_header(header_bytes, FrameCodec::kHeaderBytes, config_.limits, header);
        if (!status.ok()) {
            Frame response;
            response.header.type = MessageType::ErrorResponse;
            response.header.correlation = RequestId{};
            ByteWriter body;
            encode(body, StatusEnvelope{status.code(), status.detail()});
            response.payload = body.take();
            (void)send_frame(connection, response);
            break;
        }
        std::vector<std::uint8_t> payload;
        if (header.payload_length > 0) {
            payload.resize(header.payload_length);
            status = read_exact(socket, payload.data(), payload.size());
            if (!status.ok()) {
                break;
            }
        }
        std::vector<std::uint8_t> whole(FrameCodec::kHeaderBytes + header.payload_length);
        std::memcpy(whole.data(), header_bytes, FrameCodec::kHeaderBytes);
        if (header.payload_length > 0) {
            std::memcpy(whole.data() + FrameCodec::kHeaderBytes, payload.data(), payload.size());
        }
        Frame request;
        status = FrameCodec::decode(whole, config_.limits, request);
        if (!status.ok()) {
            Frame response;
            response.header.type = MessageType::ErrorResponse;
            response.header.correlation = header.correlation;
            ByteWriter body;
            encode(body, StatusEnvelope{status.code(), status.detail()});
            response.payload = body.take();
            (void)send_frame(connection, response);
            break;
        }

        Frame response;
        status = dispatch(connection, request, response);
        if (!status.ok()) {
            break;
        }
        status = send_frame(connection, response);
        if (!status.ok()) {
            break;
        }
        if (request.header.type == MessageType::Shutdown) {
            if (config_.shutdown_requested) {
                config_.shutdown_requested();
            }
            break;
        }
    }
    connection->closed.store(true);
    forget_connection(connection, config_.fence_workers_on_disconnect);
}

Status CoordinatorServer::send_frame(const ConnectionPtr& connection, const Frame& frame) {
    if (connection->closed.load()) {
        return Status::error(ErrorCode::ProtocolConnectionClosed, "connection is closed");
    }
    std::vector<std::uint8_t> bytes;
    Status status = FrameCodec::encode(frame, config_.limits, bytes);
    if (!status.ok()) {
        return status;
    }
    std::lock_guard<std::mutex> write_lock(connection->write_mutex);
    return write_all(connection->socket, bytes.data(), bytes.size());
}

void CoordinatorServer::register_worker(const ConnectionPtr& connection, WorkerId worker) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    connection->workers.push_back(worker);
    workers_[worker] = connection;
}

void CoordinatorServer::unregister_worker(WorkerId worker) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    workers_.erase(worker);
}

CoordinatorServer::ConnectionPtr CoordinatorServer::find_worker_connection(WorkerId worker) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto it = workers_.find(worker);
    if (it == workers_.end()) {
        return nullptr;
    }
    return it->second.lock();
}

void CoordinatorServer::forget_connection(const ConnectionPtr& connection, bool fence_workers) {
    std::vector<WorkerId> registered;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        registered = connection->workers;
        connection->workers.clear();
        for (WorkerId worker : registered) {
            const auto it = workers_.find(worker);
            if (it != workers_.end() && it->second.lock() == connection) {
                workers_.erase(it);
            }
        }
    }
    if (!fence_workers) {
        return;
    }
    for (WorkerId worker : registered) {
        // A lost connection retires the incarnation's authority. The critic may
        // reincarnate under a fresh boot identity, but the old process can never
        // publish current findings again.
        (void)coordinator_.fence_worker(worker, "worker connection was lost");
    }
}

void CoordinatorServer::notify_assignments(ReviewId review, ReviewGeneration generation) {
    Result<ReviewSnapshot> snapshot = coordinator_.query_review(review);
    if (!snapshot.ok() || snapshot.value().review.generation != generation) {
        return;
    }
    for (const CriticAssignment& assignment : snapshot.value().assignments) {
        if (!assignment.active) {
            continue;
        }
        ConnectionPtr connection = find_worker_connection(assignment.worker);
        if (connection == nullptr) {
            continue;
        }
        ByteWriter body;
        encode(body, StatusEnvelope{ErrorCode::Ok, std::string()});
        encode(body, assignment, config_.limits);
        Frame notice;
        notice.header.type = MessageType::AssignmentNotice;
        notice.header.correlation = RequestId::from_value(assignment.id.value());
        notice.payload = body.take();
        (void)send_frame(connection, notice);
    }
}

Status CoordinatorServer::dispatch(const ConnectionPtr& connection, const Frame& request, Frame& response) {
    response.header.version = FrameCodec::kProtocolVersion;
    response.header.correlation = request.header.correlation;
    response.header.authority = request.header.authority;
    response.header.flags = 0;
    response.payload.clear();

    StatusEnvelope envelope;
    ByteWriter body;
    MessageType ack_type = MessageType::ErrorResponse;
    const RuntimeLimits& limits = config_.limits;
    ByteReader reader(request.payload, payload_policy());

    switch (request.header.type) {
        case MessageType::Hello: {
            ack_type = MessageType::HelloAck;
            if (!reader.at_end()) {
                envelope = StatusEnvelope{ErrorCode::ProtocolTrailingGarbage,
                                          "handshake payload carries unexpected bytes"};
                break;
            }
            body.u64(coordinator_.epoch().value());
            break;
        }
        case MessageType::RegisterTarget: {
            ack_type = MessageType::RegisterTargetAck;
            RegisterTargetRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<TargetRecord> result = coordinator_.register_target(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::SupersedeTarget: {
            ack_type = MessageType::SupersedeTargetAck;
            SupersedeTargetRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<TargetRecord> result = coordinator_.supersede_target(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::RegisterCritic: {
            ack_type = MessageType::RegisterCriticAck;
            RegisterCriticRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<CriticRegistration> result = coordinator_.register_critic(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::RegisterWorker: {
            ack_type = MessageType::RegisterWorkerAck;
            RegisterWorkerRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<WorkerRecord> result = coordinator_.register_worker(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            register_worker(connection, result.value().id);
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::DeclareReadiness: {
            ack_type = MessageType::DeclareReadinessAck;
            DeclareReadinessRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<WorkerRecord> result = coordinator_.declare_readiness(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            register_worker(connection, result.value().id);
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::DeclareReview: {
            ack_type = MessageType::DeclareReviewAck;
            DeclareReviewRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            Result<ReviewRecord> result = coordinator_.declare_review(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::AssignCritics: {
            ack_type = MessageType::AssignCriticsAck;
            AssignCriticsRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<std::vector<CriticAssignment>> result = coordinator_.assign_critics(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            body.u32(static_cast<std::uint32_t>(result.value().size()));
            for (const CriticAssignment& assignment : result.value()) {
                encode(body, assignment, limits);
            }
            // Every assigned worker incarnation is told about its assignment
            // immediately, so a worker never has to poll the coordinator.
            notify_assignments(typed.review, typed.review_generation);
            break;
        }
        case MessageType::SubmitEvidence: {
            ack_type = MessageType::SubmitEvidenceAck;
            SubmitEvidenceRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<EvidenceRecord> result = coordinator_.submit_evidence(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::SubmitFinding: {
            ack_type = MessageType::SubmitFindingAck;
            SubmitFindingRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<Finding> result = coordinator_.submit_finding(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::SubmitChallenge: {
            ack_type = MessageType::SubmitChallengeAck;
            SubmitChallengeRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<Challenge> result = coordinator_.submit_challenge(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::SubmitRebuttal: {
            ack_type = MessageType::SubmitRebuttalAck;
            SubmitRebuttalRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<Rebuttal> result = coordinator_.submit_rebuttal(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::CommitReview: {
            ack_type = MessageType::CommitReviewAck;
            CommitReviewRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<Decision> result = coordinator_.commit_review(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::CancelReview: {
            ack_type = MessageType::CancelReviewAck;
            CancelReviewRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            typed.epoch = request.header.authority.epoch;
            typed.request = request.header.correlation;
            Result<ReviewRecord> result = coordinator_.cancel_review(typed);
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::QueryReview: {
            ack_type = MessageType::QueryReviewAck;
            const std::uint64_t review = reader.u64();
            const std::uint64_t generation = reader.u64();
            if (!reader.ok()) {
                envelope = StatusEnvelope{reader.code(), reader.detail()};
                break;
            }
            if (!reader.at_end()) {
                envelope = StatusEnvelope{ErrorCode::ProtocolTrailingGarbage,
                                          "query payload carries bytes after the declared body"};
                break;
            }
            Result<ReviewSnapshot> result =
                coordinator_.query_review(ReviewId::from_value(review));
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            if (generation != 0 && result.value().review.generation.value() != generation) {
                envelope = StatusEnvelope{ErrorCode::StaleReviewGeneration,
                                          "query named a review generation that is not current"};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::QueryDecision: {
            ack_type = MessageType::QueryDecisionAck;
            const std::uint64_t review = reader.u64();
            const std::uint64_t generation = reader.u64();
            if (!reader.ok()) {
                envelope = StatusEnvelope{reader.code(), reader.detail()};
                break;
            }
            if (!reader.at_end()) {
                envelope = StatusEnvelope{ErrorCode::ProtocolTrailingGarbage,
                                          "query payload carries bytes after the declared body"};
                break;
            }
            Result<Decision> result = coordinator_.query_decision(
                ReviewId::from_value(review),
                generation == 0 ? ReviewGeneration{} : ReviewGeneration::from_value(generation));
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::QueryAssignments: {
            ack_type = MessageType::QueryAssignmentsAck;
            const std::uint64_t worker = reader.u64();
            if (!reader.ok()) {
                envelope = StatusEnvelope{reader.code(), reader.detail()};
                break;
            }
            if (!reader.at_end()) {
                envelope = StatusEnvelope{ErrorCode::ProtocolTrailingGarbage,
                                          "query payload carries bytes after the declared body"};
                break;
            }
            std::vector<CriticAssignment> collected;
            for (const ReviewSummary& summary : coordinator_.list_reviews()) {
                Result<ReviewSnapshot> snapshot = coordinator_.query_review(summary.id);
                if (!snapshot.ok()) {
                    continue;
                }
                for (const CriticAssignment& assignment : snapshot.value().assignments) {
                    if (assignment.worker.value() == worker && assignment.active) {
                        collected.push_back(assignment);
                    }
                }
            }
            body.u32(static_cast<std::uint32_t>(collected.size()));
            for (const CriticAssignment& assignment : collected) {
                encode(body, assignment, limits);
            }
            break;
        }
        case MessageType::QueryTarget: {
            ack_type = MessageType::QueryTargetAck;
            const std::uint64_t identifier = reader.u64();
            if (!reader.ok()) {
                envelope = StatusEnvelope{reader.code(), reader.detail()};
                break;
            }
            if (!reader.at_end()) {
                envelope = StatusEnvelope{ErrorCode::ProtocolTrailingGarbage,
                                          "query payload carries bytes after the declared body"};
                break;
            }
            Result<TargetRecord> result = coordinator_.query_target(TargetId::from_value(identifier));
            if (!result.ok()) {
                envelope = StatusEnvelope{result.code(), result.detail()};
                break;
            }
            encode(body, result.value(), limits);
            break;
        }
        case MessageType::QueryWorkers: {
            ack_type = MessageType::QueryWorkersAck;
            const std::vector<WorkerRecord> records = coordinator_.list_workers();
            body.u32(static_cast<std::uint32_t>(records.size()));
            for (const WorkerRecord& record : records) {
                encode(body, record, limits);
            }
            break;
        }
        case MessageType::ListReviews: {
            ack_type = MessageType::ListReviewsAck;
            const std::vector<ReviewSummary> summaries = coordinator_.list_reviews();
            body.u32(static_cast<std::uint32_t>(summaries.size()));
            for (const ReviewSummary& summary : summaries) {
                encode(body, summary, limits);
            }
            break;
        }
        case MessageType::FenceWorker: {
            ack_type = MessageType::FenceWorkerAck;
            FenceWorkerRequest typed;
            Status status = decode(reader, limits, typed);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            if (request.header.authority.epoch != coordinator_.epoch()) {
                envelope = StatusEnvelope{ErrorCode::StaleCoordinatorEpoch,
                                          "fence request carries a superseded coordinator epoch"};
                break;
            }
            status = coordinator_.fence_worker(typed.worker, typed.reason);
            if (!status.ok()) {
                envelope = StatusEnvelope{status.code(), status.detail()};
                break;
            }
            body.boolean(true);
            break;
        }
        case MessageType::Shutdown: {
            ack_type = MessageType::Shutdown;
            body.boolean(true);
            break;
        }
        default: {
            envelope = StatusEnvelope{ErrorCode::ProtocolUnknownMessageType,
                                      "the coordinator does not accept this message type"};
            break;
        }
    }

    if (envelope.code == ErrorCode::Ok && !reader.at_end()) {
        envelope = StatusEnvelope{ErrorCode::ProtocolTrailingGarbage,
                                  "request payload carries bytes after the declared body"};
    }

    if (response.header.correlation.valid() == false && request.header.correlation.valid()) {
        response.header.correlation = request.header.correlation;
    }
    if (envelope.code != ErrorCode::Ok) {
        response.header.type = MessageType::ErrorResponse;
        ByteWriter error_body;
        encode(error_body, envelope);
        response.payload = error_body.take();
    } else {
        response.header.type = ack_type;
        ByteWriter ok_body;
        encode(ok_body, StatusEnvelope{ErrorCode::Ok, std::string()});
        const std::vector<std::uint8_t> head = ok_body.take();
        response.payload = head;
        const std::vector<std::uint8_t> tail = body.take();
        response.payload.insert(response.payload.end(), tail.begin(), tail.end());
    }
    response.header.payload_length = static_cast<std::uint32_t>(response.payload.size());
    return Status::success();
}

// --- CoordinatorClient -------------------------------------------------------

CoordinatorClient::~CoordinatorClient() { close(); }

Status CoordinatorClient::connect(const std::string& host, std::uint16_t port, const RuntimeLimits& limits) {
    limits_ = limits;
    limits_.clamp_to_hard_ceiling();
    Status status = SocketRuntime::ensure_initialised();
    if (!status.ok()) {
        return status;
    }
    native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidNative) {
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::InternalError, "unable to create a client socket");
    }
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close_native(socket);
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::InternalError, "client host is not a valid IPv4 literal");
    }
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_native(socket);
        SocketRuntime::shutdown();
        return Status::error(ErrorCode::ProtocolConnectionClosed, "unable to connect to the coordinator");
    }
    set_no_delay(socket);
    socket_ = from_native(socket);
    connected_.store(true);
    return Status::success();
}

void CoordinatorClient::close() {
    if (socket_ == static_cast<std::uintptr_t>(-1)) {
        return;
    }
    const native_socket socket = to_native(socket_);
    socket_ = static_cast<std::uintptr_t>(-1);
    connected_.store(false);
    close_native(socket);
    SocketRuntime::shutdown();
}

void CoordinatorClient::set_push_handler(PushHandler handler) { push_handler_ = std::move(handler); }

Status CoordinatorClient::send(MessageType type, const AuthorityContext& authority,
                               const std::vector<std::uint8_t>& payload) {
    if (!connected_.load()) {
        return Status::error(ErrorCode::ProtocolConnectionClosed, "client is not connected");
    }
    Frame frame;
    frame.header.type = type;
    frame.header.correlation = authority.request;
    frame.header.authority = authority;
    frame.payload = payload;
    std::vector<std::uint8_t> bytes;
    Status status = FrameCodec::encode(frame, limits_, bytes);
    if (!status.ok()) {
        return status;
    }
    return write_all(to_native(socket_), bytes.data(), bytes.size());
}

Status CoordinatorClient::send_raw(const std::vector<std::uint8_t>& bytes) {
    if (!connected_.load()) {
        return Status::error(ErrorCode::ProtocolConnectionClosed, "client is not connected");
    }
    if (bytes.size() > limits_.max_frame_bytes) {
        return Status::error(ErrorCode::ProtocolOversized, "raw write exceeds the configured frame bound");
    }
    return write_all(to_native(socket_), bytes.data(), bytes.size());
}

Result<Frame> CoordinatorClient::receive() {
    if (!connected_.load()) {
        return Status::error(ErrorCode::ProtocolConnectionClosed, "client is not connected");
    }
    const native_socket socket = to_native(socket_);
    std::uint8_t header_bytes[FrameCodec::kHeaderBytes];
    Status status = read_exact(socket, header_bytes, FrameCodec::kHeaderBytes);
    if (!status.ok()) {
        connected_.store(false);
        return status;
    }
    FrameHeader header;
    status = FrameCodec::decode_header(header_bytes, FrameCodec::kHeaderBytes, limits_, header);
    if (!status.ok()) {
        return status;
    }
    std::vector<std::uint8_t> buffer(FrameCodec::kHeaderBytes + header.payload_length);
    std::memcpy(buffer.data(), header_bytes, FrameCodec::kHeaderBytes);
    if (header.payload_length > 0) {
        status = read_exact(socket, buffer.data() + FrameCodec::kHeaderBytes, header.payload_length);
        if (!status.ok()) {
            connected_.store(false);
            return status;
        }
    }
    Frame frame;
    status = FrameCodec::decode(buffer, limits_, frame);
    if (!status.ok()) {
        return status;
    }
    return frame;
}

Result<Frame> CoordinatorClient::exchange(MessageType type, const AuthorityContext& authority,
                                          const std::vector<std::uint8_t>& payload,
                                          MessageType expected_ack) {
    Status status = send(type, authority, payload);
    if (!status.ok()) {
        return status;
    }
    for (;;) {
        Result<Frame> frame = receive();
        if (!frame.ok()) {
            return frame.status();
        }
        if (frame.value().header.type == MessageType::ErrorResponse) {
            return frame.value();
        }
        if (frame.value().header.type == expected_ack) {
            return frame.value();
        }
        if (frame.value().header.type == MessageType::AssignmentNotice) {
            if (push_handler_) {
                push_handler_(frame.value());
                continue;
            }
            continue;
        }
        return Status::error(ErrorCode::ProtocolError, "unexpected response message type");
    }
}

Result<Frame> CoordinatorClient::transact(MessageType type, const AuthorityContext& authority,
                                          const std::vector<std::uint8_t>& payload,
                                          MessageType expected_ack) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    return exchange(type, authority, payload, expected_ack);
}

Status CoordinatorClient::hello(RequestId correlation) {
    AuthorityContext authority;
    authority.request = correlation;
    Result<Frame> frame = transact(MessageType::Hello, authority, {}, MessageType::HelloAck);
    if (!frame.ok()) {
        return frame.status();
    }
    ReaderPolicy policy = payload_policy();
    ByteReader reader(frame.value().payload, policy);
    StatusEnvelope envelope;
    Status status = decode(reader, envelope, limits_);
    if (!status.ok()) {
        return status;
    }
    if (envelope.code != ErrorCode::Ok) {
        return Status::error(envelope.code, envelope.detail);
    }
    epoch_ = CoordinatorEpoch::from_value(reader.u64());
    return reader.status();
}

namespace {

AuthorityContext authority_of(const RegisterTargetRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    return authority;
}

AuthorityContext authority_of(const SupersedeTargetRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    return authority;
}

AuthorityContext authority_of(const RegisterCriticRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    return authority;
}

AuthorityContext authority_of(const RegisterWorkerRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.worker = request.id;
    authority.worker_boot = request.boot;
    authority.critic = request.critic;
    authority.critic_generation = request.critic_generation;
    return authority;
}

AuthorityContext authority_of(const DeclareReadinessRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.worker = request.id;
    authority.worker_boot = request.boot;
    authority.critic = request.critic;
    authority.critic_generation = request.critic_generation;
    return authority;
}

AuthorityContext authority_of(const DeclareReviewRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.target = request.target;
    authority.target_generation = request.target_generation;
    return authority;
}

AuthorityContext authority_of(const AssignCriticsRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.review = request.review;
    authority.review_generation = request.review_generation;
    return authority;
}

AuthorityContext authority_of(const SubmitEvidenceRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.review = request.review;
    authority.review_generation = request.review_generation;
    authority.target = request.target;
    authority.target_generation = request.target_generation;
    authority.critic = request.critic;
    authority.critic_generation = request.critic_generation;
    authority.worker = request.worker;
    authority.worker_boot = request.worker_boot;
    return authority;
}

AuthorityContext authority_of(const SubmitFindingRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.review = request.review;
    authority.review_generation = request.review_generation;
    authority.target = request.target;
    authority.target_generation = request.target_generation;
    authority.critic = request.critic;
    authority.critic_generation = request.critic_generation;
    authority.worker = request.worker;
    authority.worker_boot = request.worker_boot;
    return authority;
}

AuthorityContext authority_of(const SubmitChallengeRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.review = request.review;
    authority.review_generation = request.review_generation;
    authority.target = request.target;
    authority.target_generation = request.target_generation;
    authority.critic = request.critic;
    authority.critic_generation = request.critic_generation;
    authority.worker = request.worker;
    authority.worker_boot = request.worker_boot;
    return authority;
}

AuthorityContext authority_of(const SubmitRebuttalRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.review = request.review;
    authority.review_generation = request.review_generation;
    authority.target = request.target;
    authority.target_generation = request.target_generation;
    authority.critic = request.critic;
    authority.critic_generation = request.critic_generation;
    authority.worker = request.worker;
    authority.worker_boot = request.worker_boot;
    return authority;
}

AuthorityContext authority_of(const CommitReviewRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.review = request.review;
    authority.review_generation = request.review_generation;
    authority.target_generation = request.target_generation;
    return authority;
}

AuthorityContext authority_of(const CancelReviewRequest& request) {
    AuthorityContext authority;
    authority.epoch = request.epoch;
    authority.request = request.request;
    authority.review = request.review;
    authority.review_generation = request.review_generation;
    return authority;
}

}  // namespace

#define CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(NAME, REQ_TYPE, MSG, ACK, RESULT_TYPE)     \
    Result<RESULT_TYPE> CoordinatorClient::NAME(const REQ_TYPE& request) {             \
        ByteWriter body;                                                               \
        encode(body, request, limits_);                                                \
        if (!body.ok()) {                                                              \
            return body.status();                                                      \
        }                                                                              \
        Result<Frame> frame = transact(MSG, authority_of(request), body.take(), ACK);   \
        if (!frame.ok()) {                                                             \
            return frame.status();                                                     \
        }                                                                              \
        return decode_typed_response<RESULT_TYPE>(frame.value(), limits_);             \
    }

CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(register_target, RegisterTargetRequest, MessageType::RegisterTarget,
                                    MessageType::RegisterTargetAck, TargetRecord)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(supersede_target, SupersedeTargetRequest, MessageType::SupersedeTarget,
                                    MessageType::SupersedeTargetAck, TargetRecord)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(register_critic, RegisterCriticRequest, MessageType::RegisterCritic,
                                    MessageType::RegisterCriticAck, CriticRegistration)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(register_worker, RegisterWorkerRequest, MessageType::RegisterWorker,
                                    MessageType::RegisterWorkerAck, WorkerRecord)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(declare_readiness, DeclareReadinessRequest, MessageType::DeclareReadiness,
                                    MessageType::DeclareReadinessAck, WorkerRecord)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(declare_review, DeclareReviewRequest, MessageType::DeclareReview,
                                    MessageType::DeclareReviewAck, ReviewRecord)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(submit_evidence, SubmitEvidenceRequest, MessageType::SubmitEvidence,
                                    MessageType::SubmitEvidenceAck, EvidenceRecord)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(submit_finding, SubmitFindingRequest, MessageType::SubmitFinding,
                                    MessageType::SubmitFindingAck, Finding)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(submit_challenge, SubmitChallengeRequest, MessageType::SubmitChallenge,
                                    MessageType::SubmitChallengeAck, Challenge)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(submit_rebuttal, SubmitRebuttalRequest, MessageType::SubmitRebuttal,
                                    MessageType::SubmitRebuttalAck, Rebuttal)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(commit_review, CommitReviewRequest, MessageType::CommitReview,
                                    MessageType::CommitReviewAck, Decision)
CRITIC_FABRIC_DEFINE_CLIENT_REQUEST(cancel_review, CancelReviewRequest, MessageType::CancelReview,
                                    MessageType::CancelReviewAck, ReviewRecord)

#undef CRITIC_FABRIC_DEFINE_CLIENT_REQUEST

Result<ReviewSnapshot> CoordinatorClient::query_review(ReviewId review, ReviewGeneration generation) {
    ByteWriter body;
    body.u64(review.value());
    body.u64(generation.value());
    AuthorityContext authority;
    Result<Frame> frame =
        transact(MessageType::QueryReview, authority, body.take(), MessageType::QueryReviewAck);
    if (!frame.ok()) {
        return frame.status();
    }
    return decode_typed_response<ReviewSnapshot>(frame.value(), limits_);
}

Result<Decision> CoordinatorClient::query_decision(ReviewId review, ReviewGeneration generation) {
    ByteWriter body;
    body.u64(review.value());
    body.u64(generation.value());
    AuthorityContext authority;
    Result<Frame> frame =
        transact(MessageType::QueryDecision, authority, body.take(), MessageType::QueryDecisionAck);
    if (!frame.ok()) {
        return frame.status();
    }
    return decode_typed_response<Decision>(frame.value(), limits_);
}

Result<std::vector<ReviewSummary>> CoordinatorClient::list_reviews() {
    AuthorityContext authority;
    Result<Frame> frame =
        transact(MessageType::ListReviews, authority, {}, MessageType::ListReviewsAck);
    if (!frame.ok()) {
        return frame.status();
    }
    return decode_list_response<ReviewSummary>(frame.value(), limits_, 4096);
}

Result<TargetRecord> CoordinatorClient::query_target(TargetId target) {
    ByteWriter body;
    body.u64(target.value());
    AuthorityContext authority;
    Result<Frame> frame =
        transact(MessageType::QueryTarget, authority, body.take(), MessageType::QueryTargetAck);
    if (!frame.ok()) {
        return frame.status();
    }
    return decode_typed_response<TargetRecord>(frame.value(), limits_);
}

Result<std::vector<WorkerRecord>> CoordinatorClient::list_workers() {
    AuthorityContext authority;
    Result<Frame> frame =
        transact(MessageType::QueryWorkers, authority, {}, MessageType::QueryWorkersAck);
    if (!frame.ok()) {
        return frame.status();
    }
    return decode_list_response<WorkerRecord>(frame.value(), limits_, 4096);
}

Result<std::vector<CriticAssignment>> CoordinatorClient::query_assignments(WorkerId worker) {
    ByteWriter body;
    body.u64(worker.value());
    AuthorityContext authority;
    Result<Frame> frame = transact(MessageType::QueryAssignments, authority, body.take(),
                                   MessageType::QueryAssignmentsAck);
    if (!frame.ok()) {
        return frame.status();
    }
    return decode_list_response<CriticAssignment>(frame.value(), limits_, 4096);
}

Result<std::vector<CriticAssignment>> CoordinatorClient::assign_critics(const AssignCriticsRequest& request) {
    ByteWriter body;
    encode(body, request, limits_);
    if (!body.ok()) {
        return body.status();
    }
    Result<Frame> frame = transact(MessageType::AssignCritics, authority_of(request), body.take(),
                                   MessageType::AssignCriticsAck);
    if (!frame.ok()) {
        return frame.status();
    }
    return decode_list_response<CriticAssignment>(frame.value(), limits_, 4096);
}

Status CoordinatorClient::fence_worker(WorkerId worker, const std::string& reason, RequestId correlation) {
    FenceWorkerRequest request;
    request.worker = worker;
    request.reason = reason;
    request.epoch = epoch_;
    request.request = correlation;
    ByteWriter body;
    encode(body, request, limits_);
    AuthorityContext authority;
    authority.epoch = epoch_;
    authority.request = correlation;
    Result<Frame> frame =
        transact(MessageType::FenceWorker, authority, body.take(), MessageType::FenceWorkerAck);
    if (!frame.ok()) {
        return frame.status();
    }
    ByteReader reader(frame.value().payload, payload_policy());
    StatusEnvelope envelope;
    Status status = decode(reader, envelope, limits_);
    if (!status.ok()) {
        return status;
    }
    if (envelope.code != ErrorCode::Ok) {
        return Status::error(envelope.code, envelope.detail);
    }
    return Status::success();
}

Status CoordinatorClient::shutdown_server(RequestId correlation) {
    AuthorityContext authority;
    authority.request = correlation;
    AuthorityContext scoped = authority;
    scoped.epoch = epoch_;
    Result<Frame> frame = transact(MessageType::Shutdown, scoped, {}, MessageType::Shutdown);
    if (!frame.ok()) {
        return frame.status();
    }
    return Status::success();
}

#undef CRITIC_FABRIC_CLIENT_REQUEST

}  // namespace critic_fabric

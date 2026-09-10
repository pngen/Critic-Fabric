// Critic Fabric — content digests and integrity checksums.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_DIGEST_HPP
#define CRITIC_FABRIC_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace critic_fabric {

// SHA-256 content digest. Used for target content identity, evidence integrity,
// capability documents, and identical-commit idempotency.
class Digest {
public:
    static constexpr std::size_t kBytes = 32;

    Digest() = default;

    [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::array<std::uint8_t, kBytes>& bytes() noexcept { return bytes_; }

    [[nodiscard]] bool is_zero() const noexcept;
    [[nodiscard]] std::string to_hex() const;

    // Returns false when the text is not exactly 64 lowercase-or-uppercase hex
    // characters. Input is rejected otherwise; nothing is guessed.
    static bool from_hex(std::string_view text, Digest& out);

    friend bool operator==(const Digest& a, const Digest& b) noexcept { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const Digest& a, const Digest& b) noexcept { return a.bytes_ != b.bytes_; }
    friend bool operator<(const Digest& a, const Digest& b) noexcept { return a.bytes_ < b.bytes_; }

private:
    std::array<std::uint8_t, kBytes> bytes_{};
};

// Streaming SHA-256 over arbitrary byte input.
class Sha256 {
public:
    Sha256() noexcept;
    void update(const std::uint8_t* data, std::size_t len) noexcept;
    void update(std::string_view text) noexcept;
    void update(const std::vector<std::uint8_t>& data) noexcept;
    [[nodiscard]] Digest finalize() noexcept;

    static Digest hash(const std::uint8_t* data, std::size_t len) noexcept;
    static Digest hash(std::string_view text) noexcept;
    static Digest hash(const std::vector<std::uint8_t>& data) noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t buffer_len_;
    std::uint64_t total_len_;
};

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) used as the transport
// frame checksum and as the persistence record integrity field.
std::uint32_t crc32(const std::uint8_t* data, std::size_t len) noexcept;
std::uint32_t crc32(std::string_view text) noexcept;
std::uint32_t crc32(const std::vector<std::uint8_t>& data) noexcept;

// Continues a CRC-32 computation from a previously finalized value, so a
// running checksum can be carried across independently encoded records.
std::uint32_t crc32_append(std::uint32_t previous, const std::uint8_t* data, std::size_t len) noexcept;
std::uint32_t crc32_append(std::uint32_t previous, const std::vector<std::uint8_t>& data) noexcept;

// Convenience: digest over a bounded byte buffer.
Digest digest_bytes(const std::vector<std::uint8_t>& data) noexcept;

}  // namespace critic_fabric

namespace std {
template <>
struct hash<critic_fabric::Digest> {
    size_t operator()(const critic_fabric::Digest& d) const noexcept {
        size_t h = 1469598103934665603ULL;
        for (std::uint8_t b : d.bytes()) {
            h ^= static_cast<size_t>(b);
            h *= 1099511628211ULL;
        }
        return h;
    }
};
}  // namespace std

#endif  // CRITIC_FABRIC_DIGEST_HPP

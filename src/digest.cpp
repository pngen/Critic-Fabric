// Critic Fabric — SHA-256 and CRC-32 implementations (no third-party code).
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/digest.hpp"

#include <cstring>

namespace critic_fabric {
namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32u - n));
}

constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}
constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}
constexpr std::uint32_t choose(std::uint32_t e, std::uint32_t f, std::uint32_t g) noexcept {
    return (e & f) ^ (~e & g);
}
constexpr std::uint32_t majority(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept {
    return (a & b) ^ (a & c) ^ (b & c);
}

std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<std::uint8_t>(v & 0xFFu);
}

void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (56 - 8 * i)) & 0xFFu);
    }
}

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

bool Digest::is_zero() const noexcept {
    for (std::uint8_t b : bytes_) {
        if (b != 0) {
            return false;
        }
    }
    return true;
}

std::string Digest::to_hex() const {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(kBytes * 2);
    for (std::size_t i = 0; i < kBytes; ++i) {
        out[2 * i] = kHex[(bytes_[i] >> 4) & 0x0Fu];
        out[2 * i + 1] = kHex[bytes_[i] & 0x0Fu];
    }
    return out;
}

bool Digest::from_hex(std::string_view text, Digest& out) {
    if (text.size() != kBytes * 2) {
        return false;
    }
    Digest parsed;
    for (std::size_t i = 0; i < kBytes; ++i) {
        const int hi = hex_value(text[2 * i]);
        const int lo = hex_value(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        parsed.bytes_[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    out = parsed;
    return true;
}

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u},
      buffer_{},
      buffer_len_(0),
      total_len_(0) {}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = load_be32(block + 4 * i);
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = h + big_sigma1(e) + choose(e, f, g) + kSha256K[i] + w[i];
        const std::uint32_t t2 = big_sigma0(a) + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t len) noexcept {
    if (data == nullptr || len == 0) {
        return;
    }
    total_len_ += static_cast<std::uint64_t>(len);
    std::size_t offset = 0;
    if (buffer_len_ > 0) {
        const std::size_t need = 64 - buffer_len_;
        const std::size_t take = len < need ? len : need;
        std::memcpy(buffer_ + buffer_len_, data, take);
        buffer_len_ += take;
        offset += take;
        if (buffer_len_ == 64) {
            compress(buffer_);
            buffer_len_ = 0;
        }
    }
    while (len - offset >= 64) {
        compress(data + offset);
        offset += 64;
    }
    if (offset < len) {
        std::memcpy(buffer_ + buffer_len_, data + offset, len - offset);
        buffer_len_ += len - offset;
    }
}

void Sha256::update(std::string_view text) noexcept {
    update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void Sha256::update(const std::vector<std::uint8_t>& data) noexcept {
    update(data.data(), data.size());
}

Digest Sha256::finalize() noexcept {
    const std::uint64_t bit_len = total_len_ * 8u;
    // The padding block is either 64 or 128 bytes: appending 0x80 to a message
    // whose final block already holds 56 or more bytes needs a second block for
    // the mandatory length field.
    std::uint8_t pad[128];
    std::memcpy(pad, buffer_, buffer_len_);
    std::size_t length = buffer_len_;
    pad[length++] = 0x80u;
    const std::size_t total = length <= 56 ? 64 : 128;
    std::memset(pad + length, 0, total - length - 8);
    store_be64(pad + total - 8, bit_len);
    compress(pad);
    if (total == 128) {
        compress(pad + 64);
    }

    Digest out;
    for (int i = 0; i < 8; ++i) {
        store_be32(out.bytes().data() + 4 * i, state_[i]);
    }
    return out;
}

Digest Sha256::hash(const std::uint8_t* data, std::size_t len) noexcept {
    Sha256 h;
    h.update(data, len);
    return h.finalize();
}

Digest Sha256::hash(std::string_view text) noexcept { return hash(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }

Digest Sha256::hash(const std::vector<std::uint8_t>& data) noexcept { return hash(data.data(), data.size()); }

std::uint32_t crc32(const std::uint8_t* data, std::size_t len) noexcept {
    const auto& table = crc_table();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(std::string_view text) noexcept {
    return crc32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::uint32_t crc32(const std::vector<std::uint8_t>& data) noexcept { return crc32(data.data(), data.size()); }

std::uint32_t crc32_append(std::uint32_t previous, const std::uint8_t* data, std::size_t len) noexcept {
    const auto& table = crc_table();
    std::uint32_t c = previous ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t crc32_append(std::uint32_t previous, const std::vector<std::uint8_t>& data) noexcept {
    return crc32_append(previous, data.data(), data.size());
}

Digest digest_bytes(const std::vector<std::uint8_t>& data) noexcept { return Sha256::hash(data); }

}  // namespace critic_fabric

// Critic Fabric — review target model.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_TARGET_HPP
#define CRITIC_FABRIC_TARGET_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "critic_fabric/digest.hpp"
#include "critic_fabric/digest_domain.hpp"
#include "critic_fabric/enums.hpp"
#include "critic_fabric/ids.hpp"

namespace critic_fabric {

// Compatibility metadata travels with targets and critics so that a review can
// reject an assignment when producer or schema contracts do not line up.
struct Compatibility {
    std::string schema_name;
    std::string producer_version;

    friend bool operator==(const Compatibility& a, const Compatibility& b) {
        return a.schema_name == b.schema_name && a.producer_version == b.producer_version;
    }
    friend bool operator!=(const Compatibility& a, const Compatibility& b) { return !(a == b); }
};

// A review target is an identified, generation-bound, digest-bound object. The
// payload is bounded opaque application data; all authority derives from the
// typed metadata, never from the payload contents.
struct TargetRecord {
    TargetId id{};
    TargetGeneration generation = TargetGeneration::first();
    TargetClass target_class = TargetClass::Claim;
    Digest content_digest{};
    Digest schema_digest{};
    std::string schema_name;
    ProducerId producer{};
    std::string producer_version;
    std::string provenance;
    std::vector<std::uint8_t> payload;
    std::uint64_t created_order = 0;
    bool reviewable = true;
    TargetSupersession supersession = TargetSupersession::Current;
    TargetGeneration superseded_by{};
    std::string supersession_reason;

    // Recomputes the canonical content digest from target class, schema, and
    // payload. Metadata such as provenance is deliberately excluded so that
    // administrative edits do not masquerade as content change; changing the
    // payload or declared schema does change the digest.
    [[nodiscard]] Digest compute_content_digest() const;
};

// A request to establish or advance a target generation.
struct RegisterTargetRequest {
    TargetId id{};
    TargetClass target_class = TargetClass::Claim;
    std::string schema_name;
    std::vector<std::uint8_t> payload;
    ProducerId producer{};
    std::string producer_version;
    std::string provenance;
    bool reviewable = true;
    CoordinatorEpoch epoch{};
    RequestId request{};
};

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_TARGET_HPP

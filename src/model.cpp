// Critic Fabric — canonical digests over domain records.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/authority.hpp"
#include "critic_fabric/critic.hpp"
#include "critic_fabric/evidence.hpp"
#include "critic_fabric/finding.hpp"
#include "critic_fabric/ids.hpp"
#include "critic_fabric/serialization.hpp"
#include "critic_fabric/target.hpp"

namespace critic_fabric {

Digest TargetRecord::compute_content_digest() const {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::Target));
    writer.u32(static_cast<std::uint32_t>(target_class));
    writer.string(schema_name, kHardMaxMetadataBytes);
    writer.bytes(payload, kHardMaxPayloadBytes);
    return Sha256::hash(writer.buffer());
}

Digest EvidenceRecord::compute_content_digest() const {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::Evidence));
    writer.u32(static_cast<std::uint32_t>(source_type));
    writer.u32(static_cast<std::uint32_t>(provenance));
    writer.u32(static_cast<std::uint32_t>(integrity));
    writer.string(reference, kHardMaxMetadataBytes);
    writer.bytes(payload, kHardMaxPayloadBytes);
    return Sha256::hash(writer.buffer());
}

Digest Finding::compute_finding_digest() const {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::Finding));
    write_id(writer, critic);
    writer.u64(critic_generation.value());
    write_id(writer, assignment);
    write_id(writer, review);
    writer.u64(review_generation.value());
    write_id(writer, target);
    writer.u64(target_generation.value());
    writer.u32(static_cast<std::uint32_t>(category));
    writer.u32(static_cast<std::uint32_t>(severity));
    writer.u32(static_cast<std::uint32_t>(outcome));
    writer.u32(static_cast<std::uint32_t>(evidence.size()));
    for (EvidenceId evidence_id : evidence) {
        writer.u64(evidence_id.value());
    }
    writer.string(explanation, kHardMaxPayloadBytes);
    writer.u64(attempt.value());
    return Sha256::hash(writer.buffer());
}

std::string AuthorityContext::to_string() const {
    std::string out;
    out.reserve(160);
    out += "epoch=";
    out += std::to_string(epoch.value());
    out += " worker=";
    out += std::to_string(worker.value());
    out += " boot=";
    out += std::to_string(worker_boot.value());
    out += " critic=";
    out += std::to_string(critic.value());
    out += "/";
    out += std::to_string(critic_generation.value());
    out += " review=";
    out += std::to_string(review.value());
    out += "/";
    out += std::to_string(review_generation.value());
    out += " target=";
    out += std::to_string(target.value());
    out += "/";
    out += std::to_string(target_generation.value());
    out += " request=";
    out += std::to_string(request.value());
    return out;
}

}  // namespace critic_fabric

// Critic Fabric — deterministic explanation rendering and digesting.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/explanation.hpp"

#include <string>

#include "critic_fabric/digest_domain.hpp"
#include "critic_fabric/limits.hpp"
#include "critic_fabric/serialization.hpp"

namespace critic_fabric {
namespace {

constexpr std::uint32_t kExplanationEncodingVersion = 1;

void append_kv(std::string& out, const char* key, const std::string& value) {
    out += "  ";
    out += key;
    out += ": ";
    out += value;
    out += "\n";
}

void append_kv(std::string& out, const char* key, std::uint64_t value) {
    append_kv(out, key, std::to_string(value));
}

void append_id_list(std::string& out, const char* key, const std::vector<CriticId>& values) {
    std::string joined;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            joined += " ";
        }
        joined += "#" + std::to_string(values[i].value());
    }
    append_kv(out, key, joined.empty() ? std::string("(none)") : joined);
}

template <typename Id>
void append_generic_id_list(std::string& out, const char* key, const std::vector<Id>& values) {
    std::string joined;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            joined += " ";
        }
        joined += "#" + std::to_string(values[i].value());
    }
    append_kv(out, key, joined.empty() ? std::string("(none)") : joined);
}

}  // namespace

std::string Explanation::render() const {
    std::string out;
    out.reserve(2048);

    out += "review #";
    out += std::to_string(review.value());
    out += " generation ";
    out += std::to_string(review_generation.value());
    out += "\n";
    append_kv(out, "coordinator_epoch", epoch.value());
    append_kv(out, "state", to_string(state));
    append_kv(out, "disposition", to_string(disposition));
    append_kv(out, "policy_result", policy_result);

    out += "target\n";
    append_kv(out, "id", target.value());
    append_kv(out, "generation", target_generation.value());
    append_kv(out, "digest", target_digest.to_hex());
    append_kv(out, "policy_digest", policy_digest.to_hex());
    append_kv(out, "explanation_digest", explanation_digest.to_hex());
    if (decision.valid()) {
        append_kv(out, "decision", decision.value());
        append_kv(out, "decision_generation", decision_generation.value());
    } else {
        append_kv(out, "decision", std::string("(not committed)"));
    }

    out += "contributors\n";
    for (const CriticContribution& contribution : contributors) {
        out += "  critic #";
        out += std::to_string(contribution.critic.value());
        out += " generation ";
        out += std::to_string(contribution.critic_generation.value());
        out += " worker #";
        out += std::to_string(contribution.worker.value());
        out += " boot #";
        out += std::to_string(contribution.worker_boot.value());
        out += " role ";
        out += to_string(contribution.role);
        out += contribution.required ? " required" : " optional";
        out += " outcome ";
        out += to_string(contribution.outcome);
        out += " severity ";
        out += to_string(contribution.severity);
        out += " weight ";
        out += std::to_string(contribution.weight);
        out += " finding #";
        out += std::to_string(contribution.finding.value());
        if (!contribution.note.empty()) {
            out += " note(";
            out += contribution.note;
            out += ")";
        }
        out += "\n";
    }

    out += "predicates\n";
    for (const PredicateEvaluation& predicate : predicates) {
        out += "  ";
        out += to_string(predicate.kind);
        out += predicate.mandatory ? " mandatory" : " advisory";
        out += predicate.satisfied ? " satisfied" : " FAILED";
        out += " parameter ";
        out += std::to_string(predicate.parameter);
        out += " (";
        out += predicate.detail;
        out += ")\n";
    }

    out += "quorum\n";
    append_kv(out, "basis", quorum.basis);
    append_kv(out, "required_min_critics", quorum.min_critic_count);
    append_kv(out, "required_critics", quorum.required_critic_count);
    append_kv(out, "quorum_required", quorum.quorum);
    append_kv(out, "distinct_decisive_critics", quorum.distinct_decisive_critics);
    append_kv(out, "distinct_abstaining_critics", quorum.distinct_abstaining_critics);
    append_kv(out, "distinct_unknown_critics", quorum.distinct_unknown_critics);
    append_kv(out, "satisfied", std::string(quorum.satisfied ? "true" : "false"));
    for (CriticRole role : quorum.uncovered_required_roles) {
        append_kv(out, "uncovered_required_role", std::string(to_string(role)));
    }

    out += "disagreement\n";
    append_kv(out, "present", std::string(disagreement.present ? "true" : "false"));
    append_kv(out, "disagreeing_critics", disagreement.disagreeing_critic_count);
    append_kv(out, "policy_threshold", disagreement.policy_threshold);
    append_kv(out, "disposition", to_string(disagreement.disposition));
    append_id_list(out, "pass_critics", disagreement.pass_critics);
    append_id_list(out, "fail_critics", disagreement.fail_critics);
    append_id_list(out, "warn_critics", disagreement.warn_critics);
    append_id_list(out, "abstaining_critics", disagreement.abstaining_critics);
    append_id_list(out, "unknown_critics", disagreement.unknown_critics);
    append_id_list(out, "inconclusive_critics", disagreement.inconclusive_critics);
    append_id_list(out, "missing_critics", disagreement.missing_critics);
    append_id_list(out, "failed_critics", disagreement.failed_critics);
    for (const std::string& pair : disagreement.conflicting_pairs) {
        append_kv(out, "conflicting_pair", pair);
    }

    out += "findings\n";
    append_generic_id_list(out, "current", current_findings);
    append_generic_id_list(out, "superseded", superseded_findings);
    append_generic_id_list(out, "invalidated", invalidated_findings);
    append_generic_id_list(out, "excluded", excluded_findings);
    for (const auto& entry : exclusion_reasons) {
        out += "  excluded #";
        out += std::to_string(entry.first.value());
        out += ": ";
        out += entry.second;
        out += "\n";
    }
    append_generic_id_list(out, "evidence_used", evidence_used);
    append_kv(out, "pass_count", pass_count);
    append_kv(out, "fail_count", fail_count);
    append_kv(out, "warn_count", warn_count);
    append_kv(out, "abstain_count", abstain_count);
    append_kv(out, "unknown_count", unknown_count);
    append_kv(out, "not_applicable_count", not_applicable_count);
    append_kv(out, "inconclusive_count", inconclusive_count);
    append_kv(out, "critical_fail_count", critical_fail_count);
    append_kv(out, "max_severity", std::string(to_string(max_severity)));

    out += "challenges\n";
    append_generic_id_list(out, "challenges", challenges);
    append_generic_id_list(out, "unresolved_challenges", unresolved_challenges);

    out += "notes\n";
    for (const std::string& note : notes) {
        out += "  ";
        out += note;
        out += "\n";
    }

    out += "confidence\n";
    append_kv(out, "decision_authority", std::string("none (advisory metadata only)"));
    return out;
}

Digest compute_explanation_digest(const Explanation& explanation) {
    ByteWriter writer;
    writer.u32(static_cast<std::uint32_t>(ContentDigestDomain::Explanation));
    writer.u32(kExplanationEncodingVersion);
    writer.u64(explanation.review.value());
    writer.u64(explanation.review_generation.value());
    writer.u64(explanation.target.value());
    writer.u64(explanation.target_generation.value());
    writer.digest(explanation.target_digest);
    // The lifecycle position is deliberately excluded: the same authoritative
    // content must yield the same digest before and after the result commits,
    // otherwise an idempotent duplicate commit would look like a conflict.
    writer.u32(static_cast<std::uint32_t>(explanation.disposition));
    writer.u64(explanation.epoch.value());
    writer.digest(explanation.policy_digest);
    writer.string(explanation.policy_result, kHardMaxMetadataBytes);

    writer.u32(static_cast<std::uint32_t>(explanation.contributors.size()));
    for (const CriticContribution& contribution : explanation.contributors) {
        writer.u64(contribution.critic.value());
        writer.u64(contribution.critic_generation.value());
        writer.u64(contribution.worker.value());
        writer.u64(contribution.worker_boot.value());
        writer.u32(static_cast<std::uint32_t>(contribution.role));
        writer.boolean(contribution.required);
        writer.boolean(contribution.decisive);
        writer.u64(contribution.finding.value());
        writer.u32(static_cast<std::uint32_t>(contribution.outcome));
        writer.u32(static_cast<std::uint32_t>(contribution.severity));
        writer.u32(contribution.weight);
        writer.string(contribution.note, kHardMaxMetadataBytes);
    }

    writer.u32(static_cast<std::uint32_t>(explanation.predicates.size()));
    for (const PredicateEvaluation& predicate : explanation.predicates) {
        writer.u32(static_cast<std::uint32_t>(predicate.kind));
        writer.boolean(predicate.mandatory);
        writer.boolean(predicate.satisfied);
        writer.u32(predicate.parameter);
        writer.string(predicate.detail, kHardMaxMetadataBytes);
    }

    writer.u32(explanation.quorum.min_critic_count);
    writer.u32(explanation.quorum.required_critic_count);
    writer.u32(explanation.quorum.quorum);
    writer.u32(explanation.quorum.distinct_decisive_critics);
    writer.u32(explanation.quorum.distinct_abstaining_critics);
    writer.u32(explanation.quorum.distinct_unknown_critics);
    writer.u32(explanation.quorum.required_roles_decisive);
    writer.boolean(explanation.quorum.satisfied);
    writer.u32(static_cast<std::uint32_t>(explanation.quorum.uncovered_required_roles.size()));
    for (CriticRole role : explanation.quorum.uncovered_required_roles) {
        writer.u32(static_cast<std::uint32_t>(role));
    }

    writer.boolean(explanation.disagreement.present);
    writer.u32(explanation.disagreement.disagreeing_critic_count);
    writer.u32(explanation.disagreement.policy_threshold);
    writer.u32(static_cast<std::uint32_t>(explanation.disagreement.disposition));

    writer.u32(static_cast<std::uint32_t>(explanation.current_findings.size()));
    for (FindingId id : explanation.current_findings) {
        writer.u64(id.value());
    }
    writer.u32(static_cast<std::uint32_t>(explanation.superseded_findings.size()));
    for (FindingId id : explanation.superseded_findings) {
        writer.u64(id.value());
    }
    writer.u32(static_cast<std::uint32_t>(explanation.invalidated_findings.size()));
    for (FindingId id : explanation.invalidated_findings) {
        writer.u64(id.value());
    }
    writer.u32(static_cast<std::uint32_t>(explanation.excluded_findings.size()));
    for (FindingId id : explanation.excluded_findings) {
        writer.u64(id.value());
        const auto it = explanation.exclusion_reasons.find(id);
        writer.string(it == explanation.exclusion_reasons.end() ? std::string() : it->second,
                      kHardMaxMetadataBytes);
    }
    writer.u32(static_cast<std::uint32_t>(explanation.evidence_used.size()));
    for (EvidenceId id : explanation.evidence_used) {
        writer.u64(id.value());
    }

    writer.u32(explanation.pass_count);
    writer.u32(explanation.fail_count);
    writer.u32(explanation.warn_count);
    writer.u32(explanation.abstain_count);
    writer.u32(explanation.unknown_count);
    writer.u32(explanation.not_applicable_count);
    writer.u32(explanation.inconclusive_count);
    writer.u32(explanation.critical_fail_count);
    writer.u32(static_cast<std::uint32_t>(explanation.max_severity));

    writer.u32(static_cast<std::uint32_t>(explanation.challenges.size()));
    for (ChallengeId id : explanation.challenges) {
        writer.u64(id.value());
    }
    writer.u32(static_cast<std::uint32_t>(explanation.unresolved_challenges.size()));
    for (ChallengeId id : explanation.unresolved_challenges) {
        writer.u64(id.value());
    }

    writer.u64(explanation.decision.value());
    writer.u64(explanation.decision_generation.value());

    return Sha256::hash(writer.buffer());
}

}  // namespace critic_fabric

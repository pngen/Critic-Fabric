// Critic Fabric — identity, digest, codec, lifecycle, and policy unit tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <set>
#include <string>
#include <vector>

#include "critic_fabric/digest.hpp"
#include "critic_fabric/lifecycle.hpp"
#include "critic_fabric/policy.hpp"
#include "critic_fabric/protocol.hpp"
#include "critic_fabric/serialization.hpp"
#include "test_support.hpp"

using namespace critic_fabric;

CF_TEST(sha256_matches_published_vectors) {
    CF_CHECK_EQ(Sha256::hash(std::string_view("")).to_hex(),
                std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CF_CHECK_EQ(Sha256::hash(std::string_view("abc")).to_hex(),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CF_CHECK_EQ(
        Sha256::hash(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).to_hex(),
        std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // Multi-block input exercises the streaming buffer path.
    std::string large(1000000, 'a');
    CF_CHECK_EQ(Sha256::hash(std::string_view(large)).to_hex(),
                std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

CF_TEST(crc32_matches_published_vector_and_continues) {
    CF_CHECK_EQ(crc32(std::string_view("123456789")), 0xCBF43926u);
    const std::string prefix = "12345";
    const std::string suffix = "6789";
    const std::uint32_t partial = crc32(prefix);
    CF_CHECK_EQ(crc32_append(partial, reinterpret_cast<const std::uint8_t*>(suffix.data()), suffix.size()),
                0xCBF43926u);
}

CF_TEST(digest_hex_round_trip_and_rejection) {
    const Digest digest = Sha256::hash(std::string_view("round trip"));
    Digest parsed;
    CF_CHECK(Digest::from_hex(digest.to_hex(), parsed));
    CF_CHECK(parsed == digest);
    CF_CHECK(!Digest::from_hex("nothex", parsed));
    CF_CHECK(!Digest::from_hex(std::string(63, 'a'), parsed));
    CF_CHECK(!Digest::from_hex(std::string(65, 'a'), parsed));
    CF_CHECK(!Digest::from_hex(std::string(64, 'z'), parsed));
}

CF_TEST(strong_identities_do_not_collapse_into_one_primitive) {
    static_assert(!std::is_same_v<TargetId, CriticId>);
    static_assert(!std::is_same_v<ReviewGeneration, TargetGeneration>);
    static_assert(!std::is_same_v<WorkerBootId, WorkerId>);
    CF_CHECK(!TargetId{}.valid());
    CF_CHECK(TargetId::from_value(1).valid());
    const TargetGeneration first = TargetGeneration::first();
    CF_CHECK_EQ(first.advance().value(), 2u);
    CF_CHECK(!TargetGeneration{}.valid());
}

CF_TEST(enum_parsing_is_exact_and_rejects_unknown_names) {
    Severity severity{};
    CF_CHECK(parse_severity("CRITICAL", severity));
    CF_CHECK(severity == Severity::Critical);
    CF_CHECK(!parse_severity("critical", severity));
    CF_CHECK(!parse_severity("SEVERE", severity));
    CF_CHECK(!is_valid_severity(0));
    CF_CHECK(!is_valid_severity(6));
    FindingOutcome outcome{};
    CF_CHECK(parse_finding_outcome("UNKNOWN", outcome));
    CF_CHECK(outcome == FindingOutcome::Unknown);
    CF_CHECK(is_unknown_outcome(outcome));
    CF_CHECK(!is_decisive_outcome(outcome));
    CF_CHECK(!is_decisive_outcome(FindingOutcome::Abstain));
    CF_CHECK(is_decisive_outcome(FindingOutcome::Fail));
}

CF_TEST(lifecycle_rejects_impossible_transitions) {
    CF_CHECK(validate_review_transition(ReviewState::Declared, ReviewState::Assigning).ok());
    CF_CHECK(validate_review_transition(ReviewState::Declared, ReviewState::Committed).code() ==
             ErrorCode::InvalidStateTransition);
    CF_CHECK(validate_review_transition(ReviewState::Cancelled, ReviewState::Committed).code() ==
             ErrorCode::InvalidStateTransition);
    CF_CHECK(validate_review_transition(ReviewState::Retired, ReviewState::Declared).code() ==
             ErrorCode::InvalidStateTransition);
    CF_CHECK(validate_review_transition(ReviewState::Evaluating, ReviewState::Evaluating).code() ==
             ErrorCode::InvalidStateTransition);
    CF_CHECK(!accepts_critic_input(ReviewState::Cancelled));
    CF_CHECK(!accepts_critic_input(ReviewState::Committed));
    CF_CHECK(accepts_critic_input(ReviewState::Reviewing));
    CF_CHECK(is_terminal_review_state(ReviewState::Committed));
    CF_CHECK(!is_recoverable_review_state(ReviewState::Reviewing));
    CF_CHECK(is_recoverable_review_state(ReviewState::Committed));
}

CF_TEST(review_spec_validation_rejects_incoherent_policies) {
    ReviewSpec spec = default_review_spec();
    CF_CHECK(validate_review_spec(spec).ok());

    ReviewSpec broken = spec;
    broken.quorum = broken.min_critic_count + 1;
    CF_CHECK(validate_review_spec(broken).code() == ErrorCode::PolicyRejected);

    broken = spec;
    broken.confidence_has_authority = true;
    CF_CHECK(validate_review_spec(broken).code() == ErrorCode::PolicyRejected);

    broken = spec;
    broken.required_critic_count = 2;
    CF_CHECK(validate_review_spec(broken).code() == ErrorCode::PolicyRejected);

    broken = spec;
    broken.challenges = ChallengePolicy::Disabled;
    broken.max_challenge_rounds = 1;
    CF_CHECK(validate_review_spec(broken).code() == ErrorCode::PolicyRejected);

    broken = spec;
    broken.aggregation = AggregationPolicy::PolicyTable;
    broken.policy_table.clear();
    CF_CHECK(validate_review_spec(broken).code() == ErrorCode::PolicyRejected);

    broken = spec;
    broken.critic_weights[CriticId::from_value(3)] = 0;
    CF_CHECK(validate_review_spec(broken).code() == ErrorCode::PolicyRejected);

    broken = spec;
    broken.default_weight = 0;
    CF_CHECK(validate_review_spec(broken).code() == ErrorCode::PolicyRejected);
}

CF_TEST(serialization_is_canonical_and_bounded) {
    TargetRecord target;
    target.id = TargetId::from_value(11);
    target.generation = TargetGeneration::first();
    target.target_class = TargetClass::Artifact;
    target.schema_name = "test/schema";
    target.payload = {1, 2, 3, 4};

    ByteWriter first;
    encode(first, target);
    ByteWriter second;
    encode(second, target);
    CF_CHECK(first.buffer() == second.buffer());

    RuntimeLimits limits;
    ByteReader reader(first.buffer());
    TargetRecord decoded;
    CF_REQUIRE(decode(reader, decoded, limits).ok());
    CF_CHECK(decoded.id == target.id);
    CF_CHECK(decoded.payload == target.payload);
    CF_CHECK(reader.at_end());
}

CF_TEST(serialization_rejects_truncation_and_invalid_enums) {
    TargetRecord target;
    target.id = TargetId::from_value(11);
    target.generation = TargetGeneration::first();
    target.target_class = TargetClass::Artifact;
    target.schema_name = "test/schema";
    target.payload = {1, 2, 3, 4};
    ByteWriter writer;
    encode(writer, target);
    const std::vector<std::uint8_t> bytes = writer.buffer();

    RuntimeLimits limits;
    for (std::size_t cut = 1; cut < bytes.size(); cut += 7) {
        ByteReader reader(bytes.data(), cut);
        TargetRecord decoded;
        const Status status = decode(reader, decoded, limits);
        CF_CHECK(!status.ok());
    }

    std::vector<std::uint8_t> corrupted = bytes;
    // target_class occupies bytes 16..19 (id u64, generation u64, then u32).
    corrupted[19] = 200;
    ByteReader reader(corrupted);
    TargetRecord decoded;
    CF_CHECK(decode(reader, decoded, limits).code() == ErrorCode::ProtocolInvalidEnum);
}

CF_TEST(byte_writer_enforces_metadata_bound) {
    ByteWriter writer;
    writer.string(std::string(5000, 'x'), 64);
    CF_CHECK(!writer.ok());
    CF_CHECK(writer.code() == ErrorCode::ResourceLimitExceeded);
}

CF_TEST(frame_codec_round_trip_and_rejections) {
    RuntimeLimits limits;
    Frame frame;
    frame.header.type = MessageType::SubmitFinding;
    frame.header.correlation = RequestId::from_value(99);
    frame.header.authority.epoch = CoordinatorEpoch::first();
    frame.header.authority.critic = CriticId::from_value(4);
    frame.payload = {9, 8, 7, 6};

    std::vector<std::uint8_t> bytes;
    CF_REQUIRE(FrameCodec::encode(frame, limits, bytes).ok());
    Frame decoded;
    CF_REQUIRE(FrameCodec::decode(bytes, limits, decoded).ok());
    CF_CHECK(decoded.header.type == MessageType::SubmitFinding);
    CF_CHECK(decoded.header.correlation == RequestId::from_value(99));
    CF_CHECK(decoded.header.authority.critic == CriticId::from_value(4));
    CF_CHECK(decoded.payload == frame.payload);

    std::vector<std::uint8_t> trailing = bytes;
    trailing.push_back(0);
    CF_CHECK(FrameCodec::decode(trailing, limits, decoded).code() == ErrorCode::ProtocolTrailingGarbage);

    std::vector<std::uint8_t> bad_magic = bytes;
    bad_magic[0] = 'X';
    CF_CHECK(FrameCodec::decode(bad_magic, limits, decoded).code() == ErrorCode::ProtocolBadMagic);

    std::vector<std::uint8_t> bad_version = bytes;
    bad_version[5] = 9;
    CF_CHECK(FrameCodec::decode(bad_version, limits, decoded).code() == ErrorCode::ProtocolBadVersion);

    std::vector<std::uint8_t> bad_enum = bytes;
    bad_enum[7] = 0xFF;
    CF_CHECK(FrameCodec::decode(bad_enum, limits, decoded).code() == ErrorCode::ProtocolUnknownMessageType);

    std::vector<std::uint8_t> bad_checksum = bytes;
    bad_checksum.back() ^= 0xFF;
    CF_CHECK(FrameCodec::decode(bad_checksum, limits, decoded).code() == ErrorCode::ProtocolBadChecksum);

    std::vector<std::uint8_t> oversized = bytes;
    oversized[12] = 0x7F;
    oversized[13] = 0xFF;
    oversized[14] = 0xFF;
    oversized[15] = 0xFF;
    CF_CHECK(FrameCodec::decode(oversized, limits, decoded).code() == ErrorCode::ProtocolOversized);

    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + 40);
    CF_CHECK(FrameCodec::decode(truncated, limits, decoded).code() == ErrorCode::ProtocolTruncated);

    std::vector<std::uint8_t> reserved_flags = bytes;
    reserved_flags[11] = 0x01;
    CF_CHECK(FrameCodec::decode(reserved_flags, limits, decoded).code() == ErrorCode::ProtocolError);
}

CF_TEST(frame_checksum_covers_the_header) {
    RuntimeLimits limits;
    Frame frame;
    frame.header.type = MessageType::Hello;
    frame.header.correlation = RequestId::from_value(1);
    std::vector<std::uint8_t> bytes;
    CF_REQUIRE(FrameCodec::encode(frame, limits, bytes).ok());
    // Corrupting the authority block, which is not part of the payload, must
    // still be detected.
    bytes[30] ^= 0xFF;
    Frame decoded;
    CF_CHECK(FrameCodec::decode(bytes, limits, decoded).code() == ErrorCode::ProtocolBadChecksum);
}

CF_TEST(limits_clamp_to_the_hard_ceiling) {
    RuntimeLimits limits;
    limits.max_frame_bytes = 0xFFFFFFFFu;
    limits.max_evidence_payload_bytes = 0xFFFFFFFFu;
    limits.max_critics_per_review = 0xFFFFFFFFu;
    limits.clamp_to_hard_ceiling();
    CF_CHECK(limits.max_frame_bytes <= kHardMaxFrameBytes);
    CF_CHECK(limits.max_evidence_payload_bytes <= kHardMaxPayloadBytes);
    CF_CHECK(limits.max_critics_per_review <= kHardMaxCriticsPerReview);
}

CF_TEST(confidence_is_never_authoritative) {
    CF_CHECK(!Confidence::is_authoritative());
    ReviewSpec spec = default_review_spec();
    CF_CHECK(!spec.confidence_has_authority);
    ByteWriter writer;
    encode(writer, spec);
    std::vector<std::uint8_t> bytes = writer.buffer();
    // The confidence authority flag sits near the end of the policy encoding.
    RuntimeLimits limits;
    ByteReader reader(bytes);
    ReviewSpec decoded;
    CF_REQUIRE(decode(reader, decoded, limits).ok());
    CF_CHECK(!decoded.confidence_has_authority);
}

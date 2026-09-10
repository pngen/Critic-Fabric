# Critic Fabric

Critic Fabric is an open-source, vendor-neutral C++20 reference runtime for governing reusable critique, verification, review, challenge, and evidence-backed assessment workers across autonomous AI infrastructure.

Critique is not a string. Verification is not merely "ask another model." A critic saying FAIL does not automatically make something failed, and a critic saying PASS does not prove correctness. A confidence score is not authority. A majority opinion is not necessarily correctness. A stale review must not remain authoritative after the target changes. A remediation suggestion is not permission to mutate the reviewed target.

Critic Fabric makes one distinction mechanically enforceable:

**A critic produced an opinion.**

versus:

**This exact target generation was reviewed by authoritative critic generations under current evidence, producing this current review outcome.**

That distinction is the runtime boundary.

## The systems question

Given a target claim, artifact, plan, result, model output, or execution product, which critic is authorized to review it, what evidence supports each finding, which findings remain current, how is disagreement represented, and which review outcome is authoritative enough to influence downstream systems?

## Exact boundary

Critic Fabric owns:

- critique request identity, critique target identity, and target generation binding;
- critic identity, critic generation, capability declarations, and role;
- review authority and review lifecycle;
- claim-level review, finding identity, and finding generation;
- evidence attachment, evidence provenance, and evidence integrity;
- finding category, severity, and advisory confidence;
- PASS / FAIL / WARN / ABSTAIN / UNKNOWN-style outcomes;
- structured challenge, structured rebuttal, contradictory findings, duplicate finding handling;
- review quorum, critic disagreement, and critic weighting where configured;
- verification predicates and hard eligibility checks;
- review policy evaluation and deterministic decision policy;
- remediation suggestions as non-authoritative outputs;
- review supersession, target-change invalidation, and stale result rejection;
- critic replacement, worker death and reincarnation, coordinator restart and recovery;
- deterministic explanations, durable review history, and current-versus-historical review authority;
- authoritative review-result commit.

Critic Fabric does **not** own, and does not implement:

- general multi-model ensemble orchestration, quorum of candidate models, or final candidate selection;
- global model routing;
- general agent lifecycle or model residency;
- model hosting or inference serving;
- generic workflow orchestration or generic dependency graphs;
- experiment management, trials, metrics, or rollback;
- artifact promotion;
- automatic remediation execution;
- arbitrary policy engines;
- benchmark suites, prompt management, or code generation;
- generic observability or general-purpose messaging.

Neighbouring runtime boundaries are preserved deliberately:

- **Ensemble Fabric** owns orchestration of multiple candidate models, specialists, judges, fallback paths, quorum, consensus, arbitration, and final result selection. Critic Fabric may be invoked by Ensemble Fabric; it does not orchestrate candidate models and does not select a winning candidate.
- **Model Router** decides where model work should go. Critic Fabric may require critic capabilities, but it never performs global routing.
- **Agent Runtime** owns persistent agent lifecycle. A critic may be implemented by an agent; Critic Fabric owns the review contract, not the agent execution model.
- **Experiment Fabric** owns hypotheses, branches, trials, metrics, rollback, and lineage. Critic Fabric may review experiment outputs; it is not an experiment scheduler.
- **Artifact Promotion** may consume Critic Fabric decisions as promotion evidence. Critic Fabric never promotes an artifact.

## Architecture

    controller / client  --->  coordinator  --->  critic worker processes
                          TCP               TCP

    include/critic_fabric/   public headers
    src/                     core library
    apps/                    coordinator, worker, controller, inspection CLI
    examples/                runnable examples
    bench/                   completed-operation benchmark
    tests/                   proof obligations
    downstream/              an independent consumer of the installed package

The core library builds and operates without CUDA and without any other Summon Software Labs repository. It depends only on the C++20 standard library, the platform threading library, and the platform socket library. SHA-256, CRC-32, the framing codec, the durable journal, and the synthetic reference critics are all first-party.

All authoritative mutation flows through a single coordinator path: validate authority, compute the durable journal entries, apply them through one entry-application function, then persist. There is exactly one mutation path, so replay can never drift from live behaviour.

## Identity model

Identities are strongly typed; distinct authority domains never collapse into one primitive type:

`ReviewRequestId` `ReviewId` `ReviewGeneration` `TargetId` `TargetGeneration` `CriticId` `CriticGeneration` `CriticAssignmentId` `FindingId` `FindingGeneration` `EvidenceId` `EvidenceGeneration` `ChallengeId` `ChallengeGeneration` `DecisionId` `DecisionGeneration` `WorkerId` `WorkerBootId` `CoordinatorEpoch` `AttemptGeneration` `RequestId` `ProducerId`

Generation zero means "not established"; generation one is the first established generation. Every authoritative mutable object carries explicit generation semantics.

Target generations advance when target content changes. Target metadata that does not change the content digest (provenance string, producer label) is recorded but does not advance the generation. Critic generations advance when a critic's declared capability, compatibility metadata, or implementation version changes, or when a caller explicitly requests it; a registration that only differs in provenance label is idempotent and returns the existing generation. Review generations advance on reopening. Finding generations advance on explicit supersession. Evidence generations are recorded per record. Worker boot identities are fresh for every process incarnation. The coordinator epoch advances on every coordinator start.

Historical generations remain inspectable. Only current generations may mutate current authoritative state.

## Review target model

A target carries identity, generation, target class, content digest, schema name and schema digest, producer and producer version, provenance, a bounded opaque payload, creation order, reviewability, and supersession state.

Supported target classes are `CLAIM` `ARTIFACT` `PLAN` `RESULT` `MODEL_OUTPUT` `EXECUTION_OUTPUT` `CONFIGURATION` `POLICY_RESULT` `DATASET_SLICE` `OTHER_TYPED_TARGET`. The runtime does not interpret the payload; authority derives from typed metadata only.

Advancing a target generation marks every review bound to the previous generation as `SUPERSEDED` with authority cleared and invalidates its current findings. A review that had already committed keeps its decision as a historical record for its own generation, but its authority is explicitly cleared so no consumer can mistake it for a statement about the current generation.

## Critic model

A critic is more than a socket endpoint. A critic registration carries logical identity, generation, declared roles, declared target classes, declared finding categories, an optional specialization, an implementation version, a capability-evidence digest, determinism metadata, challenge-response support, registrant, provenance, and compatibility metadata.

A physical worker incarnation carries a `WorkerId`, a fresh `WorkerBootId`, the coordinator epoch it was admitted under, the critic identity and generation it serves, a host producer identity, a process label, readiness, health, and a monotonic coordinator-assigned boot order.

Authority is generation-bound and incarnation-bound. A restarted worker never inherits authority from the previous process.

Roles implemented: `GENERAL_CRITIC` `FACT_CHECKER` `LOGIC_CRITIC` `SAFETY_CRITIC` `PERFORMANCE_CRITIC` `SECURITY_CRITIC` `CORRECTNESS_VERIFIER` `CONSISTENCY_VERIFIER` `SCHEMA_VERIFIER` `ADVERSARIAL_CRITIC` `DOMAIN_SPECIALIST` `REBUTTAL_CRITIC`.

Roles influence eligibility and review policy rather than existing as cosmetic metadata: a role must be declared by the critic's capability before it can be assigned, required roles must each produce a decisive finding before a PASS-class disposition is possible, and `roles_that_must_pass` can require specific roles to pass. New specializations are expressed through the free-form specialization string, so no persisted format change is needed to extend them.

## Incumbency versus liveness

Two different questions are deliberately kept apart.

Liveness governs the right to publish. A worker whose connection is lost, whose process is killed, or which has not declared current readiness evidence can never publish a finding, evidence record, challenge, or rebuttal. Every mutation validates liveness before touching state.

Incumbency governs whether work already published is still bound to the current authority chain for that critic identity. An assignment that was discharged by a current finding published under that exact incarnation is not retired when the incarnation later exits: the critic did its job and left. An assignment that had not been discharged is retired, and the review moves to `REVALIDATION_REQUIRED`.

A critic that is superseded by a later incarnation, or whose generation advances, does not enjoy that protection: its assignments are retired and its pending authority is cleared.

## Finding model

A finding carries identity, generation, the finding it supersedes, critic identity and generation, assignment identity, worker incarnation, attempt generation, review identity and generation, target identity and generation, category, severity, outcome, evidence references, a bounded explanation, advisory confidence, provenance, creation order, current/superseded state, the challenge that is open against it, and a content digest computed at ingest.

Finding outcomes are `PASS` `FAIL` `WARN` `ABSTAIN` `UNKNOWN` `NOT_APPLICABLE` `INCONCLUSIVE`. Severity and outcome are never conflated: a `CRITICAL`-severity finding may be unverified, and a `LOW`-severity finding may be authoritative.

One logical critic may not assert both PASS and FAIL for the same category and evidence set under one review generation; the second attempt is rejected as a conflicting finding and the earlier finding must be superseded explicitly. A byte-identical replay is governed by the duplicate policy: `REJECT` rejects it, `IDEMPOTENT_IGNORE` returns the original finding, and `KEEP_DISTINCT` records a second distinct finding.

## Evidence model

An evidence record carries identity, generation, source type, provenance, integrity, producer, target binding and target generation and target digest, review binding, critic and worker binding, an optional finding binding, a content digest computed by the coordinator, a bounded payload, a reference string, creation order, and current/superseded state.

Provenance labels are `MEASURED` `OBSERVED` `DERIVED` `REPORTED` `SYNTHETIC` `RECONSTRUCTED` `UNKNOWN`. Integrity is `VERIFIED` `UNVERIFIED` `FAILED` `UNKNOWN`.

`UNKNOWN` provenance and non-`VERIFIED` integrity can never satisfy a mandatory verification predicate. A finding may only reference evidence that is current, belongs to the same review generation, is bound to the same target generation, and was produced by the same critic generation. Evidence may only be bound to a finding produced by the same critic.

## Severity

Severity is a bounded ordinal: `INFO` `LOW` `MEDIUM` `HIGH` `CRITICAL`. There is no unbounded integer severity.

Severity affects policy through `fail_threshold` (the severity at or above which a FAIL is decisive), `warn_threshold`, `max_critical_fails`, and policy-table severity ranges. Severity never establishes truth: a critical-severity finding still has to be produced by an authoritative critic generation, be backed by usable evidence, and pass every mandatory predicate.

## Confidence semantics

Confidence is advisory metadata only. It is deliberately excluded from hard predicates, quorum computation, weighting, and the authoritative disposition, because a self-declared score has no rigorous semantics.

This is enforced rather than merely documented. `Confidence::is_authoritative()` is a compile-time `false`; `ReviewSpec::confidence_has_authority` cannot be enabled, and a policy that attempts it is rejected with `PolicyRejected` at validation and again at decode; the mandatory predicate `CONFIDENCE_NOT_AUTHORITATIVE` fails if the flag is ever observed set. Confidence influences nothing.

## Review policy

A review specification is a typed configuration model, not a DSL. It declares required and optional roles, roles that must pass, the minimum distinct decisive critic count, the required-critic count, the quorum, required finding categories with minimum counts, the maximum number of critical failures, abstention and partial-failure tolerance, fail and warn severity thresholds, whether WARN or UNKNOWN count as failure, the aggregation policy, the disagreement policy and its threshold, the duplicate policy, the challenge policy and its round bound, review and retry limits, per-critic weights, an ordered predicate list, and an optional policy table.

Aggregation policies implemented: `ALL_REQUIRED_PASS` `ANY_CRITICAL_FAIL` `QUORUM_PASS` `WEIGHTED_REVIEW` `REQUIRED_ROLE_PASS` `MAJORITY_WITH_CONSTRAINTS` `CONSENSUS_THRESHOLD` `POLICY_TABLE`. Majority vote is not the universal default; `ALL_REQUIRED_PASS` is.

Hard predicates are evaluated first, in the order the specification declares them, and their evaluation is recorded in the explanation. Implemented predicates: `EXACT_TARGET_GENERATION_MATCH` `TARGET_DIGEST_MATCH` `REQUIRED_EVIDENCE_PRESENT` `EVIDENCE_INTEGRITY_VERIFIED` `EVIDENCE_PROVENANCE_KNOWN` `REQUIRED_ROLE_PRESENT` `MINIMUM_INDEPENDENT_CRITIC_IDENTITIES` `REQUIRED_VERIFIER_PASS` `NO_CRITICAL_FAIL_FINDINGS` `CRITIC_GENERATION_CURRENT` `WORKER_BOOT_CURRENT` `COORDINATOR_EPOCH_CURRENT` `REQUIRED_CATEGORIES_COVERED` `QUORUM_SATISFIED` `CONFIDENCE_NOT_AUTHORITATIVE`.

A failed mandatory predicate is never rescued by a favourable aggregate. The disposition is derived from the primary failed predicate and the weighted score is discarded.

## Quorum semantics

The denominator is the number of distinct logical critic identities that produced a current, decisive, authoritative finding for this review generation. Abstentions, UNKNOWN, NOT_APPLICABLE and INCONCLUSIVE outcomes, missing critics, failed critics, conflicted critics, and findings from stale critic generations, stale worker boots, stale target generations, or superseded review generations are excluded from the denominator and reported separately.

- `min_critic_count` is the participation floor: distinct decisive critics that must contribute.
- `required_critic_count` is how many of the declared required roles must contribute decisively.
- `quorum` is the distinct decisive contributor count required to declare the review complete.

One logical critic contributes one vote no matter how many processes run for it. If a second worker incarnation claims the same critic identity, it becomes the incumbent and the previous incarnation is fenced; the older incarnation's traffic is rejected before any state mutation, and its undischarged assignments are retired. A second assignment for the same critic identity within one review generation is rejected outright with `DuplicateCriticIdentity`.

`absence of findings != PASS`: a review with no current decisive findings resolves to `INSUFFICIENT_EVIDENCE`, never PASS. `UNKNOWN` never satisfies a mandatory PASS predicate.

## Disagreement

Disagreement is a first-class outcome, not an error and not a tie-breaker.

The explanation reports agreeing critics by outcome, abstaining critics, UNKNOWN critics, inconclusive critics, missing critics, failed critics, conflicting PASS/FAIL/WARN pairs with counts, the number of disagreeing critics, the configured threshold, and the resulting disposition.

The disagreement policy applies on top of aggregation. A PASS aggregate is re-expressed through the policy; an inconclusive aggregate (`QUORUM_NOT_REACHED`, `TIE`, `INSUFFICIENT_EVIDENCE`, `WARN`) is also re-expressed, so genuine disagreement cannot disappear behind a quorum shortfall. A decisive critical FAIL is left alone, because it is an assertion rather than a tie.

Policies: `REPORT_AND_FAIL` (disagreement becomes `DISAGREEMENT`), `REPORT_AND_WARN` (becomes `WARN`), `THRESHOLD_THEN_FAIL` (becomes `DISAGREEMENT` once the disagreeing-critic count reaches the threshold), `ESCALATE` (always `DISAGREEMENT`).

## Challenge and rebuttal

Challenges are bounded and generation-bound. A challenge names the challenged finding and its generation, the challenger identity and generation, the worker incarnation, the review and target generations, the specific disputed predicate, the rationale, evidence, and a challenge round. A critic may not challenge its own finding. The number of challenge rounds per (review, finding) is bounded by `max_challenge_rounds`, enforced by the challenge policy.

A rebuttal is bound to the challenge and its generation, to the review and target generations, and must be authored by the critic that produced the challenged finding. Rebuttals per challenge are bounded.

Outcomes: `UPHELD` leaves the finding current, `MODIFIED` and `SUPERSEDED` mark the finding superseded so the critic must publish a new generation, `INVALIDATED` marks the finding invalid, and `UNRESOLVED` leaves the finding challenged. An unresolved challenge blocks a PASS disposition: it is never allowed to disappear behind a favourable aggregate.

There is no open-ended agent debate. The goal is systems governance, not conversational theatre.

## Authoritative review commit

At most one authoritative logical review result may be committed for a review generation. The commit boundary is explicit and enforced under the coordinator's commit lock.

Multiple critics may run physically; retries may occur; findings may conflict; critics may restart; challenges may occur. Only one current logical review result becomes authoritative.

An identical repeated commit is idempotent and returns the original decision, including its decision digest. A commit that would produce a different result for the same generation is rejected with `ConflictingReviewCommit`. A stale review generation, a stale target generation, a cancelled review, a superseded review, a closed review, and a review whose authority requires revalidation are each rejected with a distinct typed code. Physical exactly-once execution is not claimed; what is claimed is at most one authoritative logical result per review generation.

## Stale-state fencing

Every critic-originated message carries a complete authority binding: coordinator epoch, worker identity, worker boot identity, critic identity, critic generation, review identity, review generation, target identity, target generation, and request identity. All of them are validated against current state before any mutation. A message that fails any fence is rejected without side effects.

Rejection classes include `StaleCoordinatorEpoch` `StaleWorkerBoot` `StaleCriticGeneration` `StaleReviewGeneration` `StaleTargetGeneration` `CriticNotAuthoritative` `CriticNotRegistered` `WorkerNotRegistered` `DuplicateCriticIdentity` `TargetMismatch` `TargetNotReviewable` `TargetSuperseded` `EvidenceNotCurrent` `EvidenceIntegrityFailed` `EvidenceProvenanceUnknown` `ConflictingFinding` `DuplicateFinding` `MalformedFinding` `MalformedEvidence` `InvalidChallenge` `StaleChallenge` `InvalidRebuttal` `StaleRebuttal` `ChallengeLimitExceeded` `ChallengeDisabled` `QuorumNotReached` `DisagreementUnresolved` `HardPredicateFailed` `InsufficientEvidence` `RequiredCriticUnavailable` `RequiredCriticFailed` `RequiredCriticUnavailable` `ResultAlreadyCommitted` `ConflictingReviewCommit` `ReviewCancelled` `ReviewSuperseded` `ReviewClosed` `ReviewNotOpen` `RevalidationRequired` `PersistenceCorruption` `ProtocolError` `ResourceLimitExceeded` and more. Materially different failures are never collapsed into a generic `false`.

## Worker death and reincarnation

A critic worker is a real operating-system process that connects over loopback TCP. When its connection is lost — because it exited, crashed, or was killed — the coordinator fences that exact incarnation: readiness and health are cleared and its authority is withdrawn.

If the incarnation had already discharged its assignment by publishing a current finding, that finding remains current: the critic completed its work. If it had not, the assignment is retired and the review moves to `REVALIDATION_REQUIRED`.

A replacement process must register a fresh `WorkerBootId` and declare readiness carrying the capability-evidence digest registered for that critic generation. Readiness with a mismatched capability digest is refused with `CriticCapabilityMismatch`, so a new process cannot inherit the dead process's authority by accident. The review must then be reassigned under the fresh incarnation before it can produce any result. Late traffic from the retired boot is rejected before any state mutation.

This is proven by `critic_fabric_tests_multiprocess` with real processes: worker A is killed while holding immediately before publication, its boot identity is fenced, a stale finding from the old boot is rejected, worker A-prime registers a fresh boot identity and publishes fresh capability evidence, and the review completes under the new authority.

## Coordinator restart

The coordinator advances a `CoordinatorEpoch` on every start and records it durably.

On restart the new coordinator replays the durable journal, recovers review specifications, target metadata, critic registrations, findings, evidence metadata, challenges, rebuttals, and committed outcomes, and then conservatively discards everything that is dynamic:

- worker readiness, health, and authority are cleared for every recovered incarnation;
- every review that was in flight when the previous coordinator stopped is moved to `REVALIDATION_REQUIRED` with its authority explicitly cleared;
- committed review outcomes survive unchanged, because they are durable truth rather than process state;
- traffic carrying a retired epoch is rejected with `StaleCoordinatorEpoch`;
- in-flight critics cannot mutate recovered current state.

A recovered review cannot commit until it is reassigned and revalidated; committing it returns `RevalidationRequired`.

Live sockets, process liveness, worker readiness, and transient connection authority are never persisted as though they remain current.

## Persistence

The durable journal is versioned and integrity-checked:

    header  : magic "CFABJRNL" (8) | format version (u32) | reserved (u32)
    record  : type (u16) | flags (u16) | payload length (u32) | sequence (u64) | CRC-32 (u32) | payload
    trailer : magic "CFABEND!" (8) | record count (u64) | running CRC-32 over header and records (u32) | reserved (u32)

Every length is bounded, every record payload carries its own CRC-32, record sequence numbers must strictly increase, the trailer record count must match the records present, and the trailer's running checksum must cover exactly the bytes before it. The explicit end marker is what makes truncation detectable: a file whose tail was cut has no valid end marker.

Corruption, truncation, bad magic, unsupported format version, checksum mismatch, unknown record type, unknown flags, non-monotonic sequence numbers, trailing data, and over-long records are each rejected with a distinct typed code. The journal is never silently repaired.

`rewrite_all()` writes the complete entry list to a sibling temporary file and atomically replaces the journal, giving a durable commit point that does not depend on in-place append ordering.

The coordinator writes to the journal after applying a mutation to memory and before returning to the caller, with only the commit lock held. If the journal write fails the coordinator marks durable state unavailable and refuses every further authoritative mutation rather than pretending the record exists.

## Distributed reference topology

The reference deployment is a real multiprocess topology:

    critic_controller / client  ->  critic_coordinator  ->  critic_worker (one or more)

All transport is real loopback TCP with framed binary messages. Each frame carries magic, protocol version, message type, flags, a bounded payload length, a correlation identity, a complete authority binding, and a CRC-32 computed over the header that precedes the checksum plus the payload. Concurrent writes to one connection are serialized by that connection's write lock.

Rejected inputs include malformed frames, truncated frames, oversized frames, invalid enumeration values, impossible lifecycle messages, bad checksums, stale coordinator epochs, stale worker boots, stale review and target generations, invalid target bindings, duplicate conflicting messages, payloads with trailing bytes, and unknown message types.

The coordinator never performs socket I/O while holding its state lock. The server's connection registry lock is never held across socket I/O; a connection lookup yields a shared pointer that is used only after the registry lock is released.

Shutdown is explicit and not polled: the coordinator exits when its standard input reaches end of file, when "stop" is written to it, or when an administrative `Shutdown` message is received on the control connection. The main thread waits on a condition variable; it never spins.

## Determinism

Given identical authoritative inputs, Critic Fabric produces identical results:

- findings, evidence, challenges, rebuttals, assignments, targets, critics and reviews are held in ordered containers and iterated in identity order;
- unordered-container iteration is never a source of authority;
- predicate evaluation follows the declared order in the policy;
- contributor ordering, quorum computation, disagreement computation, tie handling, and explanation rendering are fixed;
- the authoritative state digest is a canonical encoding over durable state, deliberately excluding process-local authority such as the coordinator epoch, so two coordinators holding identical committed history agree;
- the decision digest covers the review and target generations, the disposition, the policy digest, and the evidence-basis digest — that is, the durable basis of the result;
- the explanation digest covers the full inspectable reasoning, including the worker incarnations that produced it.

The explanation digest deliberately excludes the review lifecycle position, so the same authoritative content yields the same digest before and after a commit; otherwise an idempotent duplicate commit would be indistinguishable from a conflict.

The decision digest deliberately excludes the explanation digest, and therefore the worker boot identities inside it. Process incarnation is genuinely part of the authoritative input — the explanation records it and the state digest reflects it — but folding it into the decision digest would make two independent reviews of the same target generation under the same policy incomparable purely because different processes happened to run them. A downstream consumer comparing decisions needs them to agree when the authority set, the policy, the evidence, and the target generation agree. The digest remains sensitive to which critic identities decided, because different critics are different authorities.

The test suite proves all three properties directly: identical authoritative inputs including pinned incarnations produce identical decision, explanation, and state digests; the same scenario re-run reproduces them exactly; and a different authority set produces a different decision digest while the disposition is unchanged.

Nondeterministic critic inputs are distinguished from deterministic orchestration: the runtime guarantees a deterministic decision given the same authoritative inputs, not that two independent model-backed critics will produce the same opinion. The synthetic reference critics are themselves deterministic.

## Deterministic explanations

Every review outcome is inspectable through a structured explanation containing the review and target identity and generation, the target digest, the coordinator epoch, the lifecycle state and disposition, every contributor with its critic generation, worker boot, role, required flag, weight, finding, outcome, severity and note, every hard predicate with its parameter, mandatory flag, satisfaction and human-readable detail, the quorum basis and counts, the disagreement report, current, superseded, invalidated and excluded findings with exclusion reasons, the evidence actually used, per-outcome finding counts, the maximum severity, the challenge and unresolved-challenge state, the policy and explanation digests, and the committed decision identity.

Excluded findings always carry the reason they were excluded — stale review generation, stale target generation, stale critic generation, stale worker boot, not current, different target, or no authoritative incarnation. Operators do not reverse-engineer review state from logs.

## Concurrency model

The coordinator owns exactly two locks and documents their order.

- `commit_mutex_` is always the outermost lock. It serializes every authoritative mutation and is the only lock held while the durable journal is written.
- `state_mutex_` is a shared mutex taken exclusively for mutations and shared for queries. It is never held across file, socket, or backend work, and no callback is invoked while it is held.

There is no third lock, so no lock-order inversion is possible. Read paths take only `state_mutex_` and never block one another. No thread is joined while holding a lock the joined thread needs. Network I/O, file I/O, and critic execution all happen outside `state_mutex_`.

Query APIs return detached value copies. No caller ever receives a reference into mutable internal state.

## Resource bounds

`RuntimeLimits` bounds critics per review, concurrent reviews, retained reviews, in-flight findings, findings per review, evidence records per finding, evidence records per review, evidence payload size, explanation size, target payload size, metadata string size, challenges per review, rebuttals per challenge, review rounds, critic retries, worker connections, retained generations, frame size, persisted records, and persistence file size.

Hard compile-time ceilings cap every one of those, and any externally supplied configuration is clamped into them before it is used for allocation. Every untrusted length field is bounded before a buffer is reserved, and size arithmetic is checked rather than allowed to wrap.

## Building

Requirements: CMake 3.24 or newer, and a C++20 compiler. MSVC 19.44, GCC 12, and Clang 15 or newer are supported.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `CRITIC_FABRIC_BUILD_TESTS` | ON | build the test suite |
| `CRITIC_FABRIC_BUILD_EXAMPLES` | ON | build the runnable examples |
| `CRITIC_FABRIC_BUILD_BENCHMARKS` | ON | build the benchmark |
| `CRITIC_FABRIC_BUILD_CUDA_PROOF` | OFF | build the optional CUDA-backed critic proof |
| `CRITIC_FABRIC_ENABLE_ASAN` | OFF | build with AddressSanitizer |

The library is built with `/W4 /WX /permissive-` on MSVC and `-Wall -Wextra -Wpedantic -Werror` elsewhere. Warning suppression is not used; causes are fixed.

## Testing

    ctest --test-dir build --output-on-failure

or run the binaries directly:

    build/critic_fabric_tests
    build/critic_fabric_tests_multiprocess        # real child processes; keep the executables side by side

The multiprocess suite locates the coordinator and worker executables next to the test binary, or in `CRITIC_FABRIC_BIN_DIR`.

Tests are proof obligations, not decoration. The suite covers identity and digest vectors, canonical encoding and decoding, enumeration validation, lifecycle transition guards, policy validation, frame codec accept/reject paths, the required end-to-end review scenarios, adversarial hardening, durable-journal corruption, truncation, bad magic, unsupported version and trailing data, recovery semantics, stale epoch rejection, worker boot replay, resource bounds, cross-critic evidence rejection, a seeded randomized review state machine, deterministic race tests, and real multiprocess deployment proofs.

No test uses a timeout. A hanging test is treated as a defect and diagnosed rather than terminated.

## Installation

    cmake --install build --prefix /your/prefix

The install provides the public headers, the library, an exported CMake target set, and a package config file. The exported target is `CriticFabric::critic_fabric`.

## Downstream consumption

`downstream/` is an independent CMake project that consumes the installed package and never compiles a Critic Fabric source file:

    cmake -S downstream -B downstream/build -DCMAKE_PREFIX_PATH=/your/prefix
    cmake --build downstream/build
    ./downstream/build/downstream_consumer

It creates a target, registers two critic identities and their worker incarnations, declares a review whose policy requires both roles and a quorum of two, submits evidence and findings, evaluates the policy, commits the result, verifies the committed decision round-trips and that a repeated commit is idempotent, advances the target generation, and verifies that the historical outcome is retained but no longer claims current authority.

## Running the reference deployment

    critic_coordinator --port 0 [--persistence PATH] [--label TEXT]
    critic_worker --port N --critic ID [--role ROLE] [--behavior BEHAVIOR] [--max N]
    critic_controller --port N <command> [--flag value ...]
    critic_inspect (--persistence PATH | --port N) [--review RID] [--verbose]

The coordinator prints one readiness line containing the bound port and the established epoch, so a supervisor learns the port without polling. `critic_controller` supports `target-create` `target-show` `review-declare` `review-assign` `review-commit` `review-cancel` `review-show` `decision-show` `list-reviews` `list-workers`.

The inspection tool is not the source of truth. It is a read-only surface over persisted state or a live coordinator, and it prints typed rejections rather than guessing.

## Examples

Each example is a runnable program that exercises the public API:

| Example | Demonstrates |
| --- | --- |
| `example_single_critic` | one authoritative critic, evidence, and a committed PASS |
| `example_multi_critic` | three independent critics agreeing under required roles and quorum |
| `example_disagreement` | disagreement exposed as a first-class outcome |
| `example_quorum_and_abstention` | abstention never counting towards a PASS |
| `example_target_supersession` | target mutation invalidating prior review authority |
| `example_critic_replacement` | generation-safe critic replacement |
| `example_challenge_rebuttal` | bounded challenge and rebuttal with an upheld finding |
| `example_cancellation` | real cancellation fencing late critic output and commit |
| `example_recovery` | durable state and conservative recovery across a restart |

## Benchmarks

`critic_bench` measures completed operations only. It never times an enqueue and calls it throughput.

| Measurement | Result |
| --- | --- |
| target registration (completed) | 20000 ops, 0.153 s, 130552 ops/s |
| critic registration (completed) | 20000 ops, 0.270 s, 74032 ops/s |
| review declaration (completed) | 2000 ops, 0.036 s, 55937 ops/s |
| full review cycles (assign + evidence + finding + commit) | 2000 ops, 0.294 s, 6797 ops/s |
| review snapshot query (completed) | 2000 ops, 0.013 s, 152480 ops/s |
| deterministic explanation (completed) | 2000 ops, 0.024 s, 81854 ops/s |
| policy evaluation (completed) | 2000 ops, 0.020 s, 99273 ops/s |
| policy evaluation over 8 findings (completed) | 200 ops, 0.003 s, 66383 ops/s |
| policy evaluation over 32 findings (completed) | 200 ops, 0.008 s, 24696 ops/s |
| policy evaluation over 128 findings (completed) | 200 ops, 0.029 s, 6832 ops/s |
| durable journal append with review declaration | 5000 ops, 4.677 s, 1069 ops/s |
| durable journal load and replay (completed) | 5002 records, 4.352 s |
| frame encode (completed) | 200000 ops, 0.295 s, 678995 ops/s |
| frame decode (completed) | 200000 ops, 0.205 s, 975981 ops/s |

The "policy evaluation" figures are measured on a coordinator that holds two thousand committed reviews, so they include the cost of resolving authority for each review against a large population. The scaling rows show evaluation cost growing approximately linearly with the number of findings in the review: 8 findings at 66383 ops/s, 32 at 24696, 128 at 6832, which is the property the scaling measurement exists to expose.

Figures are from one Release run on the machine used for closure validation and are reported without fabricated precision.

### A measured performance repair

Evaluation previously rebuilt the live authority view over every registered critic, so the cost of deciding one review grew with the size of the whole coordinator rather than with the review. Scoping the view to the critics that the review actually references changed the same measurement on the same machine from:

    policy evaluation (completed):        3091 ops/s  ->  99273 ops/s
    deterministic explanation (completed): 3094 ops/s ->  81854 ops/s
    full review cycles:                   3503 ops/s  ->   6797 ops/s

Both figures are real measurements from the benchmark in this repository, before and after the change, with the complete test suite passing on both.

## REAL, SYNTHETIC, and UNSUPPORTED

**REAL**

- real operating-system processes for the coordinator, critic workers, and the controller;
- real loopback TCP sockets carrying framed binary transport with checksums;
- real process termination, real connection loss, and real fresh process incarnation;
- real coordinator termination without a graceful shutdown followed by durable recovery;
- real CUDA allocation, host-to-device transfer, kernel execution, device synchronisation, device-to-host copy, CPU parity comparison, and device memory accounting on the installed accelerator (optional build).

**SYNTHETIC**

- all in-tree critics are deterministic synthetic reference critics. They exercise the governance contracts of the runtime — authority, quota, evidence, findings, challenge, cancellation, recovery — deterministically and without any external model service;
- synthetic critic outcomes are a function of the critic configuration, the critic generation, and the target content. They are reference workers, not a measure of real model evaluation quality, and no claim of model-quality superiority is made from any systems test;
- the accelerator proof uses a real CUDA computation, but the critic identity, readiness, and policy around it are the same reference worker semantics used elsewhere. The accelerator performs a verification computation; Critic Fabric governs the review, it is not a CUDA execution engine.

**UNSUPPORTED**

- no multi-GPU, NVLink, NVSwitch, RDMA, MIG, or remote accelerator cluster behaviour is claimed or tested;
- no physical multi-node deployment is claimed; the reference topology is loopback-only on one host;
- no provider integration, paid API, external internet access, or proprietary service is required or tested;
- no benchmark of real model output quality is performed.

## Known limitations

- The distributed reference topology is loopback TCP on a single host. Multi-host placement is not implemented and not tested. A production deployment would need transport security, peer authentication, and flow control, none of which exist here.
- The coordinator holds one durable journal per coordinator. There is no replication, no leader election, and no consensus protocol; a coordinator is a single point of authority by construction.
- The journal is append-only with an atomic full rewrite available for compaction. No automatic compaction policy is implemented.
- Identity allocation is derived from the highest observed identity at recovery, which is correct because allocation and journaling happen under the same commit lock, but it is not a distributed identity service.
- Critic execution is synchronous per assignment. There is no bounded worker pool inside one process; concurrency comes from running several worker processes.
- The policy model is deliberately a typed structure rather than a general policy language. Anything the typed model cannot express needs a new typed field, not a new expression.
- The runtime records remediation suggestions as non-authoritative outputs and never executes them; no remediation executor is implemented.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

// Critic Fabric — explicit, guarded review lifecycle.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_LIFECYCLE_HPP
#define CRITIC_FABRIC_LIFECYCLE_HPP

#include "critic_fabric/enums.hpp"
#include "critic_fabric/result.hpp"

namespace critic_fabric {

// True when no further authoritative mutation of review state is permitted.
bool is_terminal_review_state(ReviewState state) noexcept;

// True when the review may still accept findings, evidence, challenges, or
// rebuttals from authoritative critics.
bool accepts_critic_input(ReviewState state) noexcept;

// True when the review has resolved to a disposition and is awaiting commit.
bool is_resolved_review_state(ReviewState state) noexcept;

// True when the state was reached by an administrative or failure path rather
// than by normal review progress.
bool is_abnormal_review_state(ReviewState state) noexcept;

// Guarded transition check. Every legal transition is listed explicitly; every
// other pair produces InvalidStateTransition with both states named.
Status validate_review_transition(ReviewState from, ReviewState to);

// The disposition a terminal state implies, or None when the state does not
// carry one.
ReviewDisposition disposition_for_state(ReviewState state) noexcept;

// True when a durable review in this state survives recovery verbatim. A state
// that depended on live process authority returns false, which means recovery
// must move the review to REVALIDATION_REQUIRED rather than resume it on the
// strength of persisted dynamic evidence.
bool is_recoverable_review_state(ReviewState state) noexcept;

}  // namespace critic_fabric

#endif  // CRITIC_FABRIC_LIFECYCLE_HPP

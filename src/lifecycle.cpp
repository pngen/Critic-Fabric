// Critic Fabric — guarded review state machine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/lifecycle.hpp"

#include <string>

namespace critic_fabric {
namespace {

bool in(ReviewState state, std::initializer_list<ReviewState> set) noexcept {
    for (ReviewState candidate : set) {
        if (candidate == state) {
            return true;
        }
    }
    return false;
}

// Every legal transition, written out. A review may only move forward along
// these edges; anything else is a typed rejection naming both states.
bool legal_transition(ReviewState from, ReviewState to) noexcept {
    switch (from) {
        case ReviewState::Declared:
            return in(to, {ReviewState::Assigning, ReviewState::Cancelled, ReviewState::Failed,
                           ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::Assigning:
            return in(to, {ReviewState::Assigned, ReviewState::Cancelled, ReviewState::Failed,
                           ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::Assigned:
            return in(to, {ReviewState::Reviewing, ReviewState::Cancelled, ReviewState::Failed,
                           ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::Reviewing:
            return in(to, {ReviewState::FindingsReceived, ReviewState::Cancelled, ReviewState::Failed,
                           ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::FindingsReceived:
            return in(to, {ReviewState::Validating, ReviewState::Cancelled, ReviewState::Failed,
                           ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::Validating:
            return in(to, {ReviewState::Challenged, ReviewState::Evaluating, ReviewState::Cancelled,
                           ReviewState::Failed, ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::Challenged:
            return in(to, {ReviewState::RebuttalPending, ReviewState::Evaluating, ReviewState::Cancelled,
                           ReviewState::Failed, ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::RebuttalPending:
            return in(to, {ReviewState::Evaluating, ReviewState::Cancelled, ReviewState::Failed,
                           ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::Evaluating:
            return in(to, {ReviewState::Resolved, ReviewState::Cancelled, ReviewState::Failed,
                           ReviewState::Superseded, ReviewState::RevalidationRequired});
        case ReviewState::Resolved:
            return in(to, {ReviewState::Committed, ReviewState::Evaluating, ReviewState::RevalidationRequired,
                           ReviewState::Cancelled, ReviewState::Superseded, ReviewState::Failed});
        case ReviewState::RevalidationRequired:
            return in(to, {ReviewState::Assigning, ReviewState::Cancelled, ReviewState::Superseded,
                           ReviewState::Failed, ReviewState::Retired, ReviewState::Resolved});
        case ReviewState::Committed:
            return in(to, {ReviewState::Superseded, ReviewState::Retired});
        case ReviewState::Cancelled:
            return in(to, {ReviewState::Retired});
        case ReviewState::Superseded:
            return in(to, {ReviewState::Retired});
        case ReviewState::Failed:
            return in(to, {ReviewState::Retired, ReviewState::Superseded});
        case ReviewState::Retired:
            return false;
    }
    return false;
}

}  // namespace

bool is_terminal_review_state(ReviewState state) noexcept {
    return in(state, {ReviewState::Committed, ReviewState::Cancelled, ReviewState::Superseded,
                      ReviewState::Failed, ReviewState::Retired});
}

bool accepts_critic_input(ReviewState state) noexcept {
    return in(state, {ReviewState::Assigned, ReviewState::Reviewing, ReviewState::FindingsReceived,
                      ReviewState::Validating, ReviewState::Challenged, ReviewState::RebuttalPending,
                      ReviewState::Evaluating, ReviewState::RevalidationRequired});
}

bool is_resolved_review_state(ReviewState state) noexcept {
    return state == ReviewState::Resolved;
}

bool is_abnormal_review_state(ReviewState state) noexcept {
    return in(state, {ReviewState::Cancelled, ReviewState::Superseded, ReviewState::Failed,
                      ReviewState::RevalidationRequired, ReviewState::Retired});
}

Status validate_review_transition(ReviewState from, ReviewState to) {
    if (from == to) {
        return Status::error(ErrorCode::InvalidStateTransition,
                             std::string("review is already in state ") + to_string(from));
    }
    if (!legal_transition(from, to)) {
        return Status::error(ErrorCode::InvalidStateTransition,
                             std::string("illegal review transition from ") + to_string(from) + " to " +
                                 to_string(to));
    }
    return Status::success();
}

ReviewDisposition disposition_for_state(ReviewState state) noexcept {
    switch (state) {
        case ReviewState::Cancelled:
            return ReviewDisposition::Cancelled;
        case ReviewState::Superseded:
            return ReviewDisposition::Superseded;
        case ReviewState::RevalidationRequired:
            return ReviewDisposition::RevalidationRequired;
        default:
            return ReviewDisposition::None;
    }
}

bool is_recoverable_review_state(ReviewState state) noexcept {
    switch (state) {
        case ReviewState::Committed:
        case ReviewState::Cancelled:
        case ReviewState::Superseded:
        case ReviewState::Failed:
        case ReviewState::Retired:
            return true;
        default:
            // Any review that was still in flight when the coordinator stopped
            // is deliberately moved to REVALIDATION_REQUIRED rather than resumed
            // on the strength of persisted dynamic evidence.
            return false;
    }
}

}  // namespace critic_fabric

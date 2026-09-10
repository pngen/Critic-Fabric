// Critic Fabric — process-level hygiene shared by every executable.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_PLATFORM_HPP
#define CRITIC_FABRIC_PLATFORM_HPP

namespace critic_fabric::platform {

// Suppresses the CRT abort dialog, the Windows error-reporting popup, and the
// file-not-found popup. A fatal condition is reported as a process exit code
// instead of a modal window, so automated runs never block on a dialog.
void configure_process_for_tests();

}  // namespace critic_fabric::platform

#endif  // CRITIC_FABRIC_PLATFORM_HPP

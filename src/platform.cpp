// Critic Fabric — process-level hygiene.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "critic_fabric/platform.hpp"

#ifdef _WIN32
#include <crtdbg.h>
#include <cstdlib>
#include <windows.h>
#endif

namespace critic_fabric::platform {

void configure_process_for_tests() {
#ifdef _WIN32
    // Never show a modal abort, assertion, or error-reporting window.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, 0);
    _CrtSetReportMode(_CRT_ERROR, 0);
    _CrtSetReportMode(_CRT_WARN, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Route CRT assertions to stderr rather than to a dialog.
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

}  // namespace critic_fabric::platform

// Critic Fabric — bounded child-process helper for the multiprocess tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef CRITIC_FABRIC_TEST_CHILD_PROCESS_HPP
#define CRITIC_FABRIC_TEST_CHILD_PROCESS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cftest {

// Runs one bounded helper executable with an inherited stdout pipe and an
// inherited stdin pipe. No shell is involved: the executable path and argument
// vector are passed directly, so no command injection surface exists.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& executable, const std::vector<std::string>& arguments);
    // Reads one line from the child's standard output. Blocks until a line is
    // available or the child closes the pipe.
    std::string read_line();
    // True while the process is still running.
    bool running() const;
    void write_line(const std::string& line);
    void close_stdin();
    // Terminates the process without allowing it to run any further.
    void terminate();
    // Waits for exit and returns the exit code, or -1 when it was terminated.
    int wait();
    [[nodiscard]] bool started() const { return process_ != nullptr; }
    [[nodiscard]] std::uint64_t pid() const { return pid_; }

private:
    void close_handles();

    void* process_ = nullptr;   // HANDLE / pid_t holder
    void* thread_ = nullptr;
    void* stdin_write_ = nullptr;
    void* stdout_read_ = nullptr;
    std::uint64_t pid_ = 0;
    bool exited_ = false;
};

// Locates a helper executable built next to the running test binary. The
// CRITIC_FABRIC_BIN_DIR environment variable overrides the search when the test
// runner places binaries in a different directory.
std::string helper_executable(const std::string& name);

}  // namespace cftest

#endif  // CRITIC_FABRIC_TEST_CHILD_PROCESS_HPP

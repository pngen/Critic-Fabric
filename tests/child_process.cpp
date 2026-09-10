// Critic Fabric — bounded child-process helper implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "child_process.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cftest {
namespace {

std::string environment_value(const char* name) {
#ifdef _WIN32
    char buffer[1024];
    std::size_t required = 0;
    if (::getenv_s(&required, buffer, sizeof(buffer), name) == 0 && required > 1) {
        return std::string(buffer);
    }
    return std::string();
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

std::string executable_directory() {
    const std::string override_directory = environment_value("CRITIC_FABRIC_BIN_DIR");
    if (!override_directory.empty()) {
        return override_directory;
    }
#ifdef _WIN32
    char buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer, length);
    const std::size_t separator = path.find_last_of("\\/");
    return separator == std::string::npos ? std::string(".") : path.substr(0, separator);
#else
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return std::string(".");
    }
    const std::string path(buffer, static_cast<std::size_t>(length));
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? std::string(".") : path.substr(0, separator);
#endif
}

}  // namespace

std::string helper_executable(const std::string& name) {
    const std::string directory = executable_directory();
#ifdef _WIN32
    return directory + "\\" + name + ".exe";
#else
    return directory + "/" + name;
#endif
}

ChildProcess::~ChildProcess() {
    if (process_ != nullptr && !exited_) {
        terminate();
        wait();
    }
    close_handles();
}

bool ChildProcess::start(const std::string& executable, const std::vector<std::string>& arguments) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes;
    std::memset(&attributes, 0, sizeof(attributes));
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    if (CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0) == 0) {
        return false;
    }
    if (CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0) == 0) {
        CloseHandle(child_stdout_read);
        CloseHandle(child_stdout_write);
        return false;
    }
    // Only the child's ends are inherited.
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

    std::string command_line = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
        command_line += " \"";
        command_line += argument;
        command_line += "\"";
    }
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup;
    std::memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stdout_write;
    startup.hStdInput = child_stdin_read;

    PROCESS_INFORMATION information;
    std::memset(&information, 0, sizeof(information));
    const BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
    CloseHandle(child_stdout_write);
    CloseHandle(child_stdin_read);
    if (created == 0) {
        CloseHandle(child_stdout_read);
        CloseHandle(child_stdin_write);
        return false;
    }
    process_ = information.hProcess;
    thread_ = information.hThread;
    stdout_read_ = child_stdout_read;
    stdin_write_ = child_stdin_write;
    pid_ = information.dwProcessId;
    exited_ = false;
    return true;
#else
    int stdout_pipe[2];
    int stdin_pipe[2];
    if (::pipe(stdout_pipe) != 0) {
        return false;
    }
    if (::pipe(stdin_pipe) != 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return false;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
    pid_t child = 0;
    const int status = posix_spawn(&child, executable.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(stdout_pipe[1]);
    ::close(stdin_pipe[0]);
    if (status != 0) {
        ::close(stdout_pipe[0]);
        ::close(stdin_pipe[1]);
        return false;
    }
    process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(child));
    stdout_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(stdout_pipe[0]));
    stdin_write_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(stdin_pipe[1]));
    pid_ = static_cast<std::uint64_t>(child);
    exited_ = false;
    return true;
#endif
}

std::string ChildProcess::read_line() {
    std::string line;
    if (stdout_read_ == nullptr) {
        return line;
    }
#ifdef _WIN32
    HANDLE handle = static_cast<HANDLE>(stdout_read_);
    char character = 0;
    DWORD read = 0;
    while (ReadFile(handle, &character, 1, &read, nullptr) != 0 && read == 1) {
        if (character == '\n') {
            break;
        }
        if (character != '\r') {
            line.push_back(character);
        }
    }
#else
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_));
    char character = 0;
    while (::read(descriptor, &character, 1) == 1) {
        if (character == '\n') {
            break;
        }
        if (character != '\r') {
            line.push_back(character);
        }
    }
#endif
    return line;
}

bool ChildProcess::running() const {
    if (process_ == nullptr || exited_) {
        return false;
    }
#ifdef _WIN32
    const DWORD status = WaitForSingleObject(static_cast<HANDLE>(process_), 0);
    return status == WAIT_TIMEOUT;
#else
    int status = 0;
    const pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    return result == 0;
#endif
}

void ChildProcess::write_line(const std::string& line) {
    if (stdin_write_ == nullptr) {
        return;
    }
    const std::string payload = line + "\n";
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(stdin_write_), payload.data(),
              static_cast<DWORD>(payload.size()), &written, nullptr);
#else
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_));
    ssize_t ignored = ::write(descriptor, payload.data(), payload.size());
    (void)ignored;
#endif
}

void ChildProcess::close_stdin() {
    if (stdin_write_ == nullptr) {
        return;
    }
#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(stdin_write_));
#else
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_)));
#endif
    stdin_write_ = nullptr;
}

void ChildProcess::terminate() {
    if (process_ == nullptr) {
        return;
    }
#ifdef _WIN32
    TerminateProcess(static_cast<HANDLE>(process_), 1);
#else
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
}

int ChildProcess::wait() {
    if (process_ == nullptr) {
        return -1;
    }
    if (exited_) {
        return -1;
    }
#ifdef _WIN32
    WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &exit_code);
    exited_ = true;
    return static_cast<int>(exit_code);
#else
    int status = 0;
    waitpid(static_cast<pid_t>(pid_), &status, 0);
    exited_ = true;
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#endif
}

void ChildProcess::close_handles() {
#ifdef _WIN32
    if (stdout_read_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdout_read_));
        stdout_read_ = nullptr;
    }
    if (stdin_write_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(stdin_write_));
        stdin_write_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (thread_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(thread_));
        thread_ = nullptr;
    }
#else
    if (stdout_read_ != nullptr) {
        ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
        stdout_read_ = nullptr;
    }
    if (stdin_write_ != nullptr) {
        ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdin_write_)));
        stdin_write_ = nullptr;
    }
    process_ = nullptr;
#endif
}

}  // namespace cftest

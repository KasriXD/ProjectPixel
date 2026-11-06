#pragma once

#include "px/core/Result.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Child processes for the platform: package checks and game servers run as
// separate programs with resource limits, so untrusted content never runs
// inside the service (docs/platform.md#security).
namespace px::platform {

struct ProcessOptions {
    std::vector<std::string> args;                            // not including the program
    std::vector<std::pair<std::string, std::string>> env;     // added to the service's environment
    std::filesystem::path outputFile;  // stdout and stderr appended here (when not capturing)
    bool capture = false;              // collect stdout and stderr (runProcess)
    std::size_t memoryLimitMiB = 0;    // 0: none. POSIX: address space; Windows: job memory
    int cpuLimitSeconds = 0;           // 0: none (POSIX only)
    // Untrusted content: the child gets only `env` and the few variables a
    // program needs to run (PATH, locale, time zone, certificate locations;
    // on Windows the system folders), never the service's other settings.
    bool cleanEnvironment = false;
    int maxOpenFiles = 0;              // 0: inherited (POSIX)
    std::size_t maxFileSizeMiB = 0;    // largest file it may write, logs included (POSIX; 0: none)
    bool singleProcess = false;        // it may not start other processes (Windows job; Linux: its sandbox)
};

class Process {
public:
    [[nodiscard]] static Result<std::unique_ptr<Process>> start(const std::filesystem::path& program,
                                                                const ProcessOptions& options);
    ~Process(); // kills the process if it is still running
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // The exit code once it has ended (signals: 128 + signal number).
    [[nodiscard]] std::optional<int> exitCode();
    [[nodiscard]] bool running() { return !exitCode().has_value(); }
    void kill();
    // Waits up to `seconds`; reads captured output meanwhile. nullopt: still running.
    [[nodiscard]] std::optional<int> wait(double seconds);
    [[nodiscard]] std::string output() const { return m_out; }
    [[nodiscard]] std::string errors() const { return m_err; }
    [[nodiscard]] long long pid() const;
    // Processor time used so far (user + system), in seconds; nullopt if unknown.
    [[nodiscard]] std::optional<double> cpuSeconds() const;

private:
    Process() = default;
    void pump(int timeoutMs);

    std::string m_out;
    std::string m_err;
    std::optional<int> m_exit;
#if defined(_WIN32)
    void* m_process = nullptr;
    void* m_job = nullptr;
    void* m_outRead = nullptr;
    void* m_errRead = nullptr;
    unsigned long m_pid = 0;
    struct Readers;
    std::unique_ptr<Readers> m_readers;
#else
    int m_pid = -1;
    int m_outFd = -1;
    int m_errFd = -1;
#endif
};

struct RunResult {
    int exitCode = -1;
    bool timedOut = false;
    std::string output;
    std::string errors;
};

// Runs a program to completion (or kills it after `timeoutSeconds`).
[[nodiscard]] Result<RunResult> runProcess(const std::filesystem::path& program, ProcessOptions options,
                                           double timeoutSeconds);

} // namespace px::platform

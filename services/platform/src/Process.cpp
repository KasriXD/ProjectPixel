#include "Process.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#endif
extern char** environ;
#endif

namespace px::platform {

using Clock = std::chrono::steady_clock;

namespace {
// Variables a child with a clean environment keeps (ProcessOptions::cleanEnvironment).
bool keptVariable(std::string_view name) {
    static constexpr std::string_view kept[] = {
        "PATH", "LANG", "LC_ALL", "LC_CTYPE", "TZ", "TMPDIR", "SSL_CERT_FILE", "SSL_CERT_DIR", "PX_PLATFORM_CA",
        "ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS",
#if defined(_WIN32)
        "SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "TEMP", "TMP", "PATHEXT", "COMSPEC", "NUMBER_OF_PROCESSORS",
        "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER", "PROGRAMDATA", "ALLUSERSPROFILE", "WINEPREFIX",
#endif
    };
    for (const auto k : kept) {
#if defined(_WIN32)
        if (name.size() == k.size() &&
            std::equal(name.begin(), name.end(), k.begin(), [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == b; })) {
            return true;
        }
#else
        if (name == k) return true;
#endif
    }
    return false;
}
} // namespace


// AddressSanitizer reserves terabytes of address space, so an address-space
// limit would stop sanitized children from starting at all.
#if defined(__SANITIZE_ADDRESS__)
#define PX_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PX_ASAN 1
#endif
#endif
#if defined(PX_ASAN)
constexpr bool kAddressLimitWorks = false;
#else
constexpr bool kAddressLimitWorks = true;
#endif

#if defined(_WIN32)
// ============================== Windows ======================================================

namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// Quoting that CommandLineToArgvW and the C runtime undo.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += *it;
    }
    cmd += L'"';
}

std::wstring environmentBlock(const std::vector<std::pair<std::string, std::string>>& extra, bool clean) {
    std::wstring block;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p != 0; p += wcslen(p) + 1) {
            std::wstring entry(p);
            if (clean) {
                const std::size_t eq = entry.find(L'=', 1);
                std::string name;
                for (std::size_t i = 0; i < eq && i < entry.size(); ++i) name += static_cast<char>(entry[i] < 128 ? entry[i] : '?');
                if (!keptVariable(name)) continue;
            }
            bool replaced = false;
            for (const auto& [k, v] : extra) {
                const std::wstring key = widen(k) + L"=";
                if (_wcsnicmp(entry.c_str(), key.c_str(), key.size()) == 0) replaced = true;
            }
            if (!replaced) {
                block += entry;
                block += L'\0';
            }
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [k, v] : extra) {
        block += widen(k) + L"=" + widen(v);
        block += L'\0';
    }
    block += L'\0';
    return block;
}
} // namespace

struct Process::Readers {
    std::thread out;
    std::thread err;
};

Result<std::unique_ptr<Process>> Process::start(const std::filesystem::path& program, const ProcessOptions& options) {
    auto p = std::unique_ptr<Process>(new Process());
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outWrite = nullptr;
    HANDLE errWrite = nullptr;
    HANDLE outRead = nullptr;
    HANDLE errRead = nullptr;
    if (options.capture) {
        if (!CreatePipe(&outRead, &outWrite, &inherit, 0) || !CreatePipe(&errRead, &errWrite, &inherit, 0)) {
            return makeError(ErrorCode::IoError, "Cannot create pipes");
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    } else if (!options.outputFile.empty()) {
        outWrite = CreateFileW(options.outputFile.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (outWrite == INVALID_HANDLE_VALUE) return makeError(ErrorCode::IoError, "Cannot open {}", options.outputFile.string());
        DuplicateHandle(GetCurrentProcess(), outWrite, GetCurrentProcess(), &errWrite, 0, TRUE, DUPLICATE_SAME_ACCESS);
    } else {
        outWrite = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        DuplicateHandle(GetCurrentProcess(), outWrite, GetCurrentProcess(), &errWrite, 0, TRUE, DUPLICATE_SAME_ACCESS);
    }
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

    // Only these three handles are inherited, not the service's sockets.
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuffer(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    HANDLE handles[3] = {nulIn, outWrite, errWrite};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nulIn;
    si.StartupInfo.hStdOutput = outWrite;
    si.StartupInfo.hStdError = errWrite;
    si.lpAttributeList = attrs;

    std::wstring cmd;
    appendQuoted(cmd, program.wstring());
    for (const auto& a : options.args) {
        cmd += L' ';
        appendQuoted(cmd, widen(a));
    }
    std::wstring env = environmentBlock(options.env, options.cleanEnvironment);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(program.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                       EXTENDED_STARTUPINFO_PRESENT,
                                   env.data(), nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(nulIn);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!ok) {
        if (outRead) CloseHandle(outRead);
        if (errRead) CloseHandle(errRead);
        return makeError(ErrorCode::IoError, "Cannot start {} (error {})", program.string(), GetLastError());
    }
    // A job: the process (and anything it starts) is killed with it and
    // limited in memory.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (options.memoryLimitMiB > 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = options.memoryLimitMiB * 1024 * 1024;
    }
    if (options.singleProcess) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
    }
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    if (options.singleProcess) {
        // No clipboard, desktop, global atoms, other processes' windows or system settings.
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
        ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                                 JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_GLOBALATOMS |
                                 JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                 JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
        SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui));
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->m_process = pi.hProcess;
    p->m_job = job;
    p->m_pid = pi.dwProcessId;
    if (options.capture) {
        p->m_outRead = outRead;
        p->m_errRead = errRead;
        p->m_readers = std::make_unique<Readers>();
        const auto reader = [](HANDLE h, std::string* sink) {
            char buffer[4096];
            DWORD n = 0;
            while (ReadFile(h, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
                if (sink->size() < (8u << 20)) sink->append(buffer, n);
            }
        };
        p->m_readers->out = std::thread(reader, outRead, &p->m_out);
        p->m_readers->err = std::thread(reader, errRead, &p->m_err);
    }
    return p;
}

Process::~Process() {
    kill();
    if (m_readers) {
        if (m_readers->out.joinable()) m_readers->out.join();
        if (m_readers->err.joinable()) m_readers->err.join();
    }
    if (m_outRead) CloseHandle(m_outRead);
    if (m_errRead) CloseHandle(m_errRead);
    if (m_process) CloseHandle(m_process);
    if (m_job) CloseHandle(m_job);
}

long long Process::pid() const { return static_cast<long long>(m_pid); }

std::optional<double> Process::cpuSeconds() const {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (m_process == nullptr || !GetProcessTimes(m_process, &created, &exited, &kernel, &user)) return std::nullopt;
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7; // 100 ns units
}

std::optional<int> Process::exitCode() {
    if (m_exit) return m_exit;
    if (WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        m_exit = static_cast<int>(code);
    }
    return m_exit;
}

void Process::kill() {
    if (exitCode()) return;
    TerminateJobObject(m_job, 137);
    WaitForSingleObject(m_process, 5000);
    (void)exitCode();
}

void Process::pump(int) {}

std::optional<int> Process::wait(double seconds) {
    const DWORD ms = seconds <= 0 ? 0 : static_cast<DWORD>(seconds * 1000.0);
    if (WaitForSingleObject(m_process, ms) == WAIT_OBJECT_0) {
        if (m_readers) {
            if (m_readers->out.joinable()) m_readers->out.join();
            if (m_readers->err.joinable()) m_readers->err.join();
        }
    }
    return exitCode();
}

#else
// =============================== POSIX =======================================================

Result<std::unique_ptr<Process>> Process::start(const std::filesystem::path& program, const ProcessOptions& options) {
    auto p = std::unique_ptr<Process>(new Process());
    // Everything the child needs is prepared before fork: after fork only
    // async-signal-safe calls are allowed.
    std::vector<std::string> argStore;
    argStore.push_back(program.string());
    for (const auto& a : options.args) argStore.push_back(a);
    std::vector<char*> argv;
    for (auto& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> envStore;
    for (char** e = environ; *e != nullptr; ++e) {
        std::string_view entry(*e);
        if (options.cleanEnvironment && !keptVariable(entry.substr(0, entry.find('=')))) continue;
        bool replaced = false;
        for (const auto& [k, v] : options.env) {
            if (entry.starts_with(k + "=")) replaced = true;
        }
        if (!replaced) envStore.emplace_back(entry);
    }
    for (const auto& [k, v] : options.env) envStore.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    int outFile = -1;
    if (options.capture) {
        if (pipe(outPipe) != 0 || pipe(errPipe) != 0) return makeError(ErrorCode::IoError, "Cannot create pipes");
    } else if (!options.outputFile.empty()) {
        outFile = open(options.outputFile.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (outFile < 0) return makeError(ErrorCode::IoError, "Cannot open {}", options.outputFile.string());
    }
    const int devNull = open("/dev/null", O_RDWR | O_CLOEXEC);
    const long maxFd = sysconf(_SC_OPEN_MAX);
    const int closeUpTo = maxFd > 0 && maxFd < 65536 ? static_cast<int>(maxFd) : 65536;
    const rlim_t memory = kAddressLimitWorks ? static_cast<rlim_t>(options.memoryLimitMiB) * 1024 * 1024 : 0;
    const rlim_t cpu = static_cast<rlim_t>(options.cpuLimitSeconds);
    const rlim_t openFiles = static_cast<rlim_t>(options.maxOpenFiles);
    const rlim_t fileSize = static_cast<rlim_t>(options.maxFileSizeMiB) * 1024 * 1024;

    const pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1], outFile, devNull}) {
            if (fd >= 0) close(fd);
        }
        return makeError(ErrorCode::IoError, "Cannot start {}", program.string());
    }
    if (pid == 0) {
        // Child.
        setpgid(0, 0); // its own process group, so kill() reaches anything it starts
        if (memory > 0) {
            struct rlimit r{memory, memory};
            setrlimit(RLIMIT_AS, &r);
        }
        if (cpu > 0) {
            struct rlimit r{cpu, cpu + 1};
            setrlimit(RLIMIT_CPU, &r);
        }
        {
            struct rlimit r{0, 0}; // no core files: they would hold the process's memory on disk
            setrlimit(RLIMIT_CORE, &r);
        }
        if (openFiles > 0) {
            struct rlimit r{openFiles, openFiles};
            setrlimit(RLIMIT_NOFILE, &r);
        }
        if (fileSize > 0) {
            struct rlimit r{fileSize, fileSize};
            setrlimit(RLIMIT_FSIZE, &r);
        }
        dup2(devNull, 0);
        if (options.capture) {
            dup2(outPipe[1], 1);
            dup2(errPipe[1], 2);
        } else if (outFile >= 0) {
            dup2(outFile, 1);
            dup2(outFile, 2);
        } else {
            dup2(devNull, 1);
            dup2(devNull, 2);
        }
        for (int fd = 3; fd < closeUpTo; ++fd) close(fd); // the service's sockets stay with the service
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    if (devNull >= 0) close(devNull);
    if (outFile >= 0) close(outFile);
    p->m_pid = pid;
    if (options.capture) {
        close(outPipe[1]);
        close(errPipe[1]);
        p->m_outFd = outPipe[0];
        p->m_errFd = errPipe[0];
        fcntl(p->m_outFd, F_SETFL, O_NONBLOCK);
        fcntl(p->m_errFd, F_SETFL, O_NONBLOCK);
        fcntl(p->m_outFd, F_SETFD, FD_CLOEXEC);
        fcntl(p->m_errFd, F_SETFD, FD_CLOEXEC);
    }
    return p;
}

Process::~Process() {
    kill();
    if (m_outFd >= 0) close(m_outFd);
    if (m_errFd >= 0) close(m_errFd);
}

long long Process::pid() const { return m_pid; }

std::optional<double> Process::cpuSeconds() const {
    if (m_pid <= 0) return std::nullopt;
#if defined(__linux__)
    // /proc/<pid>/stat: fields 14 and 15 are user and system time in clock ticks.
    const std::string path = std::format("/proc/{}/stat", m_pid);
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    char buffer[1024];
    const ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0) return std::nullopt;
    buffer[n] = 0;
    const char* p = std::strrchr(buffer, ')'); // the name may hold spaces
    if (p == nullptr) return std::nullopt;
    unsigned long long user = 0;
    unsigned long long system = 0;
    if (std::sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &user, &system) != 2) {
        return std::nullopt;
    }
    const long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? static_cast<double>(user + system) / static_cast<double>(hz) : std::optional<double>();
#elif defined(__APPLE__)
    rusage_info_v2 info{};
    if (proc_pid_rusage(m_pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&info)) != 0) return std::nullopt;
    mach_timebase_info_data_t base{};
    mach_timebase_info(&base);
    const double ns = static_cast<double>(info.ri_user_time + info.ri_system_time) * base.numer / base.denom;
    return ns * 1e-9;
#else
    return std::nullopt;
#endif
}

std::optional<int> Process::exitCode() {
    if (m_exit || m_pid <= 0) return m_exit;
    int status = 0;
    const pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == m_pid) {
        if (WIFEXITED(status)) {
            m_exit = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            m_exit = 128 + WTERMSIG(status);
        } else {
            m_exit = -1;
        }
    }
    return m_exit;
}

void Process::kill() {
    if (m_pid <= 0 || exitCode()) return;
    ::kill(-m_pid, SIGKILL); // the whole process group
    ::kill(m_pid, SIGKILL);
    int status = 0;
    if (waitpid(m_pid, &status, 0) == m_pid) {
        m_exit = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
}

void Process::pump(int timeoutMs) {
    pollfd fds[2];
    int n = 0;
    if (m_outFd >= 0) fds[n++] = {m_outFd, POLLIN, 0};
    if (m_errFd >= 0) fds[n++] = {m_errFd, POLLIN, 0};
    if (n == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        return;
    }
    if (poll(fds, static_cast<nfds_t>(n), timeoutMs) <= 0) return;
    char buffer[4096];
    for (int i = 0; i < n; ++i) {
        if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
        int& fd = fds[i].fd == m_outFd ? m_outFd : m_errFd;
        std::string& sink = fds[i].fd == m_outFd ? m_out : m_err;
        while (true) {
            const ssize_t r = read(fd, buffer, sizeof(buffer));
            if (r > 0) {
                if (sink.size() < (8u << 20)) sink.append(buffer, static_cast<std::size_t>(r));
                continue;
            }
            if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                close(fd);
                fd = -1;
            }
            break;
        }
    }
}

std::optional<int> Process::wait(double seconds) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
    while (true) {
        pump(20);
        if (exitCode() && m_outFd < 0 && m_errFd < 0) return m_exit;
        if (exitCode() && Clock::now() >= deadline) return m_exit;
        if (Clock::now() >= deadline) return std::nullopt;
    }
}
#endif

Result<RunResult> runProcess(const std::filesystem::path& program, ProcessOptions options, double timeoutSeconds) {
    options.capture = true;
    auto process = Process::start(program, options);
    if (!process) return process.error();
    RunResult result;
    const auto code = (*process)->wait(timeoutSeconds);
    if (!code) {
        result.timedOut = true;
        (*process)->kill();
        (void)(*process)->wait(1.0);
    }
    result.exitCode = (*process)->exitCode().value_or(-1);
    result.output = (*process)->output();
    result.errors = (*process)->errors();
    return result;
}

} // namespace px::platform

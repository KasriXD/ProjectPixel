#include "px/core/Sandbox.hpp"

#include <cstdlib>
#include <format>

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
#define PX_SANDBOX_LINUX 1
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

// Sanitizer runtimes start helper processes and trace the program (leak
// checks at exit); sandboxes in sanitizer builds let them.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PX_SANDBOX_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PX_SANDBOX_SANITIZED 1
#endif
#endif

namespace px::sandbox {

std::string Report::describe() const {
    if (!syscalls && landlockAbi == 0) return "not available on this system";
    std::string out = syscalls ? "system calls filtered" : "no system call filter";
    if (landlockAbi == 0) {
        out += "; Landlock not available (files and ports not limited)";
    } else {
        out += std::format("; Landlock {}: {}{}{}", landlockAbi, files ? "files read-only as listed" : "files not limited",
                           network ? ", TCP ports limited" : ", TCP ports not limited (Linux 6.7+)",
                           signals ? ", no signals out" : "");
    }
    return out;
}

#if PX_SANDBOX_LINUX

namespace {

// --- Landlock's kernel ABI (include/uapi/linux/landlock.h), spelled out so
// newer features work whatever headers the build machine has.
constexpr long kLandlockCreateRuleset = 444;
constexpr long kLandlockAddRule = 445;
constexpr long kLandlockRestrictSelf = 446;
constexpr std::uint32_t kCreateRulesetVersion = 1u << 0;
constexpr int kRulePathBeneath = 1;
constexpr int kRuleNetPort = 2;

constexpr std::uint64_t kFsExecute = 1ull << 0;
constexpr std::uint64_t kFsWriteFile = 1ull << 1;
constexpr std::uint64_t kFsReadFile = 1ull << 2;
constexpr std::uint64_t kFsReadDir = 1ull << 3;
constexpr std::uint64_t kFsV1 = (1ull << 13) - 1; // execute .. make_sym
constexpr std::uint64_t kFsRefer = 1ull << 13;    // ABI 2
constexpr std::uint64_t kFsTruncate = 1ull << 14; // ABI 3
constexpr std::uint64_t kFsIoctlDev = 1ull << 15; // ABI 5
constexpr std::uint64_t kNetBindTcp = 1ull << 0;  // ABI 4
constexpr std::uint64_t kNetConnectTcp = 1ull << 1;
constexpr std::uint64_t kScopeAbstractUnix = 1ull << 0; // ABI 6
constexpr std::uint64_t kScopeSignal = 1ull << 1;

struct RulesetAttr {
    std::uint64_t handledFs = 0;
    std::uint64_t handledNet = 0;
    std::uint64_t scoped = 0;
};

struct __attribute__((packed)) PathBeneath {
    std::uint64_t allowed;
    std::int32_t parentFd;
};

struct NetPort {
    std::uint64_t allowed;
    std::uint64_t port;
};

static_assert(kFsExecute == 1 && kFsWriteFile == 2 && kFsReadDir == 8);

int landlockAbi() {
    const long v = syscall(kLandlockCreateRuleset, nullptr, 0, kCreateRulesetVersion);
    return v < 0 ? 0 : static_cast<int>(v);
}

Status applyLandlock(const Rules& rules, Report& report) {
    const int abi = landlockAbi();
    report.landlockAbi = abi;
    if (abi <= 0) return {};
    RulesetAttr attr;
    attr.handledFs = kFsV1 | (abi >= 2 ? kFsRefer : 0) | (abi >= 3 ? kFsTruncate : 0) | (abi >= 5 ? kFsIoctlDev : 0);
    std::size_t size = sizeof(std::uint64_t);
    if (abi >= 4) {
        attr.handledNet = kNetBindTcp | kNetConnectTcp;
        size = 2 * sizeof(std::uint64_t);
    }
    if (abi >= 6) {
        attr.scoped = kScopeAbstractUnix | kScopeSignal;
        size = 3 * sizeof(std::uint64_t);
    }
    const int ruleset = static_cast<int>(syscall(kLandlockCreateRuleset, &attr, size, 0));
    if (ruleset < 0) return makeError(ErrorCode::IoError, "Landlock: cannot make a rule set ({})", std::strerror(errno));
    for (const auto& path : rules.readable) {
        const int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) continue;
        struct stat st {};
        PathBeneath rule{};
        rule.parentFd = fd;
        rule.allowed = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) ? (kFsReadFile | kFsReadDir) : kFsReadFile;
        syscall(kLandlockAddRule, ruleset, kRulePathBeneath, &rule, 0);
        close(fd);
    }
    if (abi >= 4) {
        for (const std::uint16_t port : rules.bindPorts) {
            NetPort rule{kNetBindTcp, port};
            syscall(kLandlockAddRule, ruleset, kRuleNetPort, &rule, 0);
        }
        for (const std::uint16_t port : rules.connectPorts) {
            NetPort rule{kNetConnectTcp, port};
            syscall(kLandlockAddRule, ruleset, kRuleNetPort, &rule, 0);
        }
    }
    if (syscall(kLandlockRestrictSelf, ruleset, 0) != 0) {
        const int error = errno;
        close(ruleset);
        return makeError(ErrorCode::IoError, "Landlock: cannot restrict this process ({})", std::strerror(error));
    }
    close(ruleset);
    report.files = true;
    report.network = abi >= 4;
    report.signals = abi >= 6;
    return {};
}

// --- seccomp-bpf ---------------------------------------------------------------------------

#if defined(__x86_64__)
constexpr std::uint32_t kArch = AUDIT_ARCH_X86_64;
#else
constexpr std::uint32_t kArch = AUDIT_ARCH_AARCH64;
#endif

class Program {
public:
    void load(std::uint32_t offset) { add(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offset)); }
    void ret(std::uint32_t value) { add(BPF_STMT(BPF_RET | BPF_K, value)); }
    void jeq(std::uint32_t value, std::uint8_t ifTrue, std::uint8_t ifFalse) {
        add(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, value, ifTrue, ifFalse));
    }
    void jset(std::uint32_t bits, std::uint8_t ifTrue, std::uint8_t ifFalse) {
        add(BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, bits, ifTrue, ifFalse));
    }
    void jge(std::uint32_t value, std::uint8_t ifTrue, std::uint8_t ifFalse) {
        add(BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, value, ifTrue, ifFalse));
    }
    std::vector<sock_filter> code;

private:
    void add(sock_filter f) { code.push_back(f); }
};

constexpr std::uint32_t kAllow = SECCOMP_RET_ALLOW;
constexpr std::uint32_t refuse(int error) { return SECCOMP_RET_ERRNO | (static_cast<std::uint32_t>(error) & SECCOMP_RET_DATA); }
#if defined(SECCOMP_RET_KILL_PROCESS)
constexpr std::uint32_t kKill = SECCOMP_RET_KILL_PROCESS;
#else
constexpr std::uint32_t kKill = SECCOMP_RET_KILL;
#endif
constexpr std::uint32_t argLow(int n) {
    // The low 32 bits of argument n (little-endian on both architectures).
    return static_cast<std::uint32_t>(offsetof(seccomp_data, args) + static_cast<std::size_t>(n) * 8);
}

// System calls a game server or a package check never needs and an
// attacker would want: refused with EPERM.
const std::vector<long>& refusedCalls() {
    static const std::vector<long> calls = {
        __NR_execve, __NR_execveat,
#ifdef __NR_fork
        __NR_fork,
#endif
#ifdef __NR_vfork
        __NR_vfork,
#endif
#if !defined(PX_SANDBOX_SANITIZED)
        __NR_ptrace,
#endif
        __NR_process_vm_readv, __NR_process_vm_writev, __NR_kcmp,
#ifdef __NR_pidfd_open
        __NR_pidfd_open, __NR_pidfd_getfd, __NR_pidfd_send_signal,
#endif
        __NR_tkill, __NR_mount, __NR_umount2, __NR_pivot_root, __NR_chroot, __NR_unshare, __NR_setns,
#ifdef __NR_open_tree
        __NR_open_tree, __NR_move_mount, __NR_fsopen, __NR_fsconfig, __NR_fsmount, __NR_fspick,
#endif
#ifdef __NR_mount_setattr
        __NR_mount_setattr,
#endif
        __NR_kexec_load,
#ifdef __NR_kexec_file_load
        __NR_kexec_file_load,
#endif
        __NR_init_module, __NR_finit_module, __NR_delete_module, __NR_bpf, __NR_perf_event_open, __NR_keyctl,
        __NR_add_key, __NR_request_key, __NR_userfaultfd, __NR_acct, __NR_swapon, __NR_swapoff, __NR_reboot,
        __NR_settimeofday, __NR_clock_settime, __NR_clock_adjtime, __NR_adjtimex,
#ifdef __NR_iopl
        __NR_iopl, __NR_ioperm,
#endif
        __NR_quotactl, __NR_open_by_handle_at, __NR_name_to_handle_at,
#ifdef __NR_io_uring_setup
        __NR_io_uring_setup, __NR_io_uring_enter, __NR_io_uring_register,
#endif
        __NR_setuid, __NR_setgid, __NR_setreuid, __NR_setregid, __NR_setresuid, __NR_setresgid, __NR_setgroups,
        __NR_setfsuid, __NR_setfsgid, __NR_personality, __NR_syslog, __NR_vhangup, __NR_sethostname,
        __NR_setdomainname,
    };
    return calls;
}

Program buildFilter() {
    Program p;
    // Only this architecture's calls (no 32-bit or x32 detours).
    p.load(offsetof(seccomp_data, arch));
    p.jeq(kArch, 1, 0);
    p.ret(kKill);
    p.load(offsetof(seccomp_data, nr));
#if defined(__x86_64__)
    p.jge(0x40000000u, 0, 1); // x32 calls
    p.ret(kKill);
#endif
    for (const long nr : refusedCalls()) {
        p.jeq(static_cast<std::uint32_t>(nr), 0, 1);
        p.ret(refuse(EPERM));
    }
    // clone3: "not implemented" makes the C library fall back to clone.
#ifdef __NR_clone3
    p.jeq(__NR_clone3, 0, 1);
    p.ret(refuse(ENOSYS));
#endif
#if !defined(PX_SANDBOX_SANITIZED)
    // clone: threads only.
    p.jeq(__NR_clone, 0, 4);
    p.load(argLow(0));
    p.jset(CLONE_THREAD, 1, 0);
    p.ret(refuse(EPERM));
    p.ret(kAllow);
#endif
    // Sockets: local, IPv4, IPv6 and netlink (name lookups); no raw packets.
    p.jeq(__NR_socket, 0, 7);
    p.load(argLow(0));
    p.jeq(AF_UNIX, 4, 0);
    p.jeq(AF_INET, 3, 0);
    p.jeq(AF_INET6, 2, 0);
    p.jeq(AF_NETLINK, 1, 0);
    p.ret(refuse(EAFNOSUPPORT));
    p.ret(kAllow);
    // Signals: to this process only.
    const auto pid = static_cast<std::uint32_t>(getpid());
    for (const long nr : {static_cast<long>(__NR_kill), static_cast<long>(__NR_tgkill),
                          static_cast<long>(__NR_rt_sigqueueinfo), static_cast<long>(__NR_rt_tgsigqueueinfo)}) {
        p.jeq(static_cast<std::uint32_t>(nr), 0, 4);
        p.load(argLow(0));
        p.jeq(pid, 1, 0);
        p.ret(refuse(EPERM));
        p.ret(kAllow);
    }
    p.ret(kAllow);
    return p;
}

Status applySeccomp(Report& report) {
    Program program = buildFilter();
    sock_fprog fprog{};
    fprog.len = static_cast<unsigned short>(program.code.size());
    fprog.filter = program.code.data();
    long r = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &fprog);
    if (r != 0 && errno == ENOSYS) r = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog);
    if (r != 0) {
        return makeError(ErrorCode::IoError, "Cannot install the system call filter ({})", std::strerror(errno));
    }
    report.syscalls = true;
    return {};
}

void addIfExists(std::vector<std::filesystem::path>& out, const std::filesystem::path& p) {
    std::error_code ec;
    if (!p.empty() && std::filesystem::exists(p, ec)) out.push_back(p);
}

} // namespace

bool supported() noexcept { return true; }

std::vector<std::filesystem::path> systemReadablePaths() {
    std::vector<std::filesystem::path> out;
    for (const char* p : {"/etc/hosts", "/etc/resolv.conf", "/etc/nsswitch.conf", "/etc/gai.conf", "/etc/host.conf",
                          "/etc/services", "/etc/protocols", "/etc/localtime", "/etc/ld.so.cache", "/etc/ssl",
                          "/etc/pki", "/etc/ca-certificates", "/usr/share/ca-certificates", "/usr/share/zoneinfo",
                          "/usr/lib", "/usr/lib64", "/lib", "/lib64", "/dev/urandom", "/dev/null"}) {
        addIfExists(out, p);
    }
#if defined(PX_SANDBOX_SANITIZED)
    addIfExists(out, "/proc"); // the sanitizers read the process's memory map
#endif
    for (const char* var : {"SSL_CERT_FILE", "SSL_CERT_DIR", "PX_PLATFORM_CA"}) {
        if (const char* v = std::getenv(var); v != nullptr && *v != 0) addIfExists(out, v);
    }
    return out;
}

Result<Report> enter(const Rules& rules) {
    Report report;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return makeError(ErrorCode::IoError, "Cannot set no_new_privs ({})", std::strerror(errno));
    }
    if (auto s = applyLandlock(rules, report); !s) return s.error();
    if (auto s = applySeccomp(report); !s) return s.error();
    return report;
}

#else

bool supported() noexcept { return false; }

std::vector<std::filesystem::path> systemReadablePaths() { return {}; }

Result<Report> enter(const Rules&) { return Report{}; }

#endif

} // namespace px::sandbox

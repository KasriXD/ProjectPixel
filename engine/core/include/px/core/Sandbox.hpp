#pragma once

#include "px/core/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Locking down a process that handles untrusted content (game servers and
// package checks started by the platform; docs/security.md, ADR 0031). Once
// entered, a sandbox cannot be left, and threads started later inherit it.
//
// Linux: seccomp-bpf refuses starting programs or processes, debugging or
// reading other processes, signals to other processes, kernel modules,
// mounts, namespaces, raw network access and the like; Landlock (Linux 5.13
// and later, where enabled) leaves only the listed files and folders
// readable, nothing writable, and (Linux 6.7 and later) only the listed TCP
// ports to listen on and connect to. Each part is used when the kernel has
// it; the report says what is in force. Other systems: not available (the
// platform limits those processes from outside: memory, one process).
namespace px::sandbox {

struct Rules {
    // Files and folders (with everything beneath) that stay readable.
    std::vector<std::filesystem::path> readable;
    // TCP ports this process may listen on and connect to. Empty
    // `connectPorts`: no outgoing TCP at all.
    std::vector<std::uint16_t> bindPorts;
    std::vector<std::uint16_t> connectPorts;
};

struct Report {
    bool syscalls = false;  // seccomp filter installed
    int landlockAbi = 0;    // 0: Landlock not available
    bool files = false;     // filesystem rules in force
    bool network = false;   // TCP port rules in force
    bool signals = false;   // signals to other processes refused by Landlock too
    [[nodiscard]] std::string describe() const;
};

// Whether this build can sandbox a process at all (Linux, not Android).
[[nodiscard]] bool supported() noexcept;

// What most programs need to keep reading after entering: name resolution
// (/etc/hosts, resolv.conf, nsswitch.conf and the NSS libraries), trusted
// certificates, time zones, and the files named by SSL_CERT_FILE,
// SSL_CERT_DIR and PX_PLATFORM_CA. Paths that do not exist are left out.
[[nodiscard]] std::vector<std::filesystem::path> systemReadablePaths();

// Enters the sandbox. Fails only if the system call filter cannot be
// installed where it should work; missing Landlock features are reported,
// not errors.
[[nodiscard]] Result<Report> enter(const Rules& rules);

} // namespace px::sandbox

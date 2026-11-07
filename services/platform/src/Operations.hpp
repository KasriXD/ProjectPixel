#pragma once

#include "px/core/Result.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Running the platform on the internet (docs/deployment.md): who is really
// asking behind a reverse proxy, numbers for monitoring, backups.
namespace px::platform {

// Addresses and ranges: "203.0.113.7", "10.0.0.0/8", "::1", "fd00::/8".
class AddressRanges {
public:
    [[nodiscard]] static Result<AddressRanges> parse(const std::vector<std::string>& items);
    [[nodiscard]] bool contains(std::string_view address) const;
    [[nodiscard]] bool empty() const { return m_ranges.empty(); }

private:
    struct Range {
        std::array<std::uint8_t, 16> bytes{}; // IPv4 as ::ffff:a.b.c.d
        int bits = 128;
    };
    std::vector<Range> m_ranges;
};

[[nodiscard]] bool isLoopback(std::string_view address);

// Who sent a request: the peer, or when the peer is a trusted proxy, the
// right-most address in X-Forwarded-For that is not one (the left part of
// that header is whatever the client wrote).
[[nodiscard]] std::string clientAddress(const AddressRanges& trustedProxies, std::string_view peer,
                                        std::string_view forwardedFor);

// Counters for /metrics, updated as the service works.
struct ServiceMetrics {
    std::array<std::atomic<std::uint64_t>, 6> requests{}; // by status class: [2] = 2xx ...
    std::atomic<std::uint64_t> requestMicros{0};          // time spent answering them
    std::atomic<std::uint64_t> rateLimited{0};
    std::atomic<std::uint64_t> backups{0};
    std::atomic<std::uint64_t> backupFailures{0};
    std::atomic<std::int64_t> lastBackup{0}; // Unix time of the last good backup
    std::atomic<std::uint64_t> lastBackupMillis{0};
    std::atomic<std::uint64_t> lastBackupBytes{0};
    std::atomic<std::uint64_t> certificateReloads{0};
    std::atomic<std::uint64_t> certificateReloadFailures{0};
    std::atomic<std::int64_t> certificateExpires{0}; // Unix time; 0: no HTTPS
    std::int64_t started = 0;
};

// A finished backup (a folder with platform.db, platform.key, the packages
// and pictures the database refers to, and backup.json).
struct BackupInfo {
    std::string name; // "20261106-181500"
    std::filesystem::path folder;
    std::int64_t created = 0;
    int schemaVersion = 0;
    std::string engineVersion;
    std::size_t files = 0; // packages and pictures
    std::uint64_t bytes = 0;
    double seconds = 0.0;
    std::vector<std::string> missing; // referenced files that were not there
};

// The backups in a folder, newest first.
[[nodiscard]] std::vector<BackupInfo> listBackups(const std::filesystem::path& dir);

// Copies a backup into an empty data folder and checks it.
struct RestoreReport {
    std::int64_t accounts = 0;
    std::int64_t games = 0;
    std::size_t files = 0;
    std::vector<std::string> missing;
};
[[nodiscard]] Result<RestoreReport> restoreBackup(const std::filesystem::path& backup, const std::filesystem::path& dataDir);

// process_* metrics in Prometheus' text format (Linux; elsewhere fewer).
[[nodiscard]] std::string processMetrics();

// When a PEM certificate (the first in the file) stops being valid, as Unix
// time; with `keyPem`, also checks that the key belongs to it.
[[nodiscard]] Result<std::int64_t> certificateExpiry(std::string_view certPem, std::string_view keyPem = {});

// GET {url}/healthz answers 200 (px-platform --check-health, container health checks).
[[nodiscard]] Status checkHealth(const std::string& url);

} // namespace px::platform

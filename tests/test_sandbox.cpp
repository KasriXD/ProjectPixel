// The process sandbox (px/core/Sandbox.hpp): a forked child enters it and
// tries what a compromised game server would.
#include <doctest.h>

#include "px/core/Sandbox.hpp"

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#else
constexpr bool kSanitized = false;
#endif

namespace {

// What the child found, one bit each (1: it worked).
enum Bit : unsigned {
    kEntered = 1u << 0,
    kFiles = 1u << 1,     // the report says files are limited
    kNetwork = 1u << 2,   // ... TCP ports are limited
    kReadAllowed = 1u << 3,
    kReadOther = 1u << 4,
    kWrite = 1u << 5,
    kExec = 1u << 6,
    kFork = 1u << 7,
    kThread = 1u << 8,
    kRawSocket = 1u << 9,
    kUdpSocket = 1u << 10,
    kSignalOther = 1u << 11,
    kBindAllowed = 1u << 12,
    kBindOther = 1u << 13,
    kSignalSelf = 1u << 14,
};

bool canRead(const std::filesystem::path& p) {
    const int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    close(fd);
    return true;
}

bool canBind(std::uint16_t port) {
    const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    close(s);
    return ok;
}

} // namespace

TEST_CASE("Sandbox: a sandboxed process cannot do what a compromised server would") {
    REQUIRE(px::sandbox::supported());
    const auto base = std::filesystem::temp_directory_path() / ("px-sandbox-" + std::to_string(getpid()));
    std::filesystem::create_directories(base / "allowed");
    std::filesystem::create_directories(base / "secret");
    std::ofstream(base / "allowed" / "game.txt") << "fine";
    std::ofstream(base / "secret" / "platform.key") << "do not read";
    const std::uint16_t allowedPort = 47311;
    const std::uint16_t otherPort = 47312;
    const pid_t parent = getpid();

    int pipeFds[2];
    REQUIRE(pipe(pipeFds) == 0);
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        close(pipeFds[0]);
        unsigned bits = 0;
        px::sandbox::Rules rules;
        rules.readable = px::sandbox::systemReadablePaths();
        rules.readable.push_back(base / "allowed");
        rules.bindPorts = {allowedPort};
        auto report = px::sandbox::enter(rules);
        if (report && report->syscalls) {
            bits |= kEntered;
            if (report->files) bits |= kFiles;
            if (report->network) bits |= kNetwork;
            if (canRead(base / "allowed" / "game.txt")) bits |= kReadAllowed;
            if (canRead(base / "secret" / "platform.key")) bits |= kReadOther;
            const int w = open((base / "allowed" / "new.txt").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
            if (w >= 0) {
                bits |= kWrite;
                close(w);
            }
            char* const argv[] = {const_cast<char*>("/bin/true"), nullptr};
            char* const envp[] = {nullptr};
            execve("/bin/true", argv, envp); // returns only if refused
            if (errno != EPERM) bits |= kExec;
            const pid_t grandchild = fork();
            if (grandchild == 0) _exit(0);
            if (grandchild > 0) {
                bits |= kFork;
                waitpid(grandchild, nullptr, 0);
            }
            bool threadRan = false;
            std::thread t([&] { threadRan = true; });
            t.join();
            if (threadRan) bits |= kThread;
            const int raw = socket(AF_PACKET, SOCK_RAW, 0);
            if (raw >= 0) {
                bits |= kRawSocket;
                close(raw);
            }
            const int udp = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp >= 0) {
                bits |= kUdpSocket;
                close(udp);
            }
            if (kill(parent, 0) == 0) bits |= kSignalOther;
            if (kill(getpid(), 0) == 0) bits |= kSignalSelf;
            if (canBind(allowedPort)) bits |= kBindAllowed;
            if (canBind(otherPort)) bits |= kBindOther;
        }
        (void)!write(pipeFds[1], &bits, sizeof(bits));
        _exit(0);
    }
    close(pipeFds[1]);
    unsigned bits = 0;
    REQUIRE(read(pipeFds[0], &bits, sizeof(bits)) == static_cast<ssize_t>(sizeof(bits)));
    close(pipeFds[0]);
    int status = 0;
    waitpid(child, &status, 0);
    std::filesystem::remove_all(base);

    MESSAGE("files limited: " << ((bits & kFiles) != 0) << ", ports limited: " << ((bits & kNetwork) != 0));
    REQUIRE((bits & kEntered) != 0);
    CHECK((bits & kExec) == 0);          // no new programs
    if (!kSanitized) CHECK((bits & kFork) == 0); // no new processes
    CHECK((bits & kThread) != 0);        // threads still work
    CHECK((bits & kRawSocket) == 0);     // no raw packets
    CHECK((bits & kUdpSocket) != 0);     // ordinary networking works
    CHECK((bits & kSignalOther) == 0);   // cannot signal the platform
    CHECK((bits & kSignalSelf) != 0);
    CHECK((bits & kReadAllowed) != 0);
    if ((bits & kFiles) != 0) {
        CHECK((bits & kReadOther) == 0); // the platform's files stay out of reach
        CHECK((bits & kWrite) == 0);     // nothing can be written
    }
    if ((bits & kNetwork) != 0) {
        CHECK((bits & kBindAllowed) != 0);
        CHECK((bits & kBindOther) == 0);
    }
}

#else

TEST_CASE("Sandbox: not available on this system") {
    CHECK_FALSE(px::sandbox::supported());
    auto report = px::sandbox::enter({});
    REQUIRE(report);
    CHECK_FALSE(report->syscalls);
}

#endif

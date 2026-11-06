#pragma once

#include "px/core/Result.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// Email from the platform (docs/accounts.md#email): address checks and
// password reset links. Sent over SMTP with TLS (smtps://, or smtp:// with
// STARTTLS, which is then required), from a queue on a worker thread so
// requests never wait for the mail server; or written as .eml files into a
// folder, for trying things out without a mail server.
namespace px::platform {

struct MailConfig {
    // "smtps://host:465", "smtp://host:587" (STARTTLS required) or
    // "smtp+plain://host:25" (no encryption: a relay on this computer only).
    std::string server;
    std::string user;     // AUTH (PLAIN), when the server wants it
    std::string password; // from PX_SMTP_PASSWORD, not the command line
    std::string from;     // "Pixel <noreply@example.com>" or "noreply@example.com"
    std::filesystem::path outbox; // instead of a server: messages become files here
    std::filesystem::path caFile; // trusted certificates for the server (default: the system's)
    double timeoutSeconds = 30.0;
    int attempts = 3;
    double retrySeconds = 30.0; // doubled after each failure

    [[nodiscard]] bool enabled() const { return !server.empty() || !outbox.empty(); }
};

struct Mail {
    std::string to;
    std::string subject;
    std::string text; // plain text, UTF-8
};

// A plausible address: one '@', no spaces or control characters, a dot in
// the domain, at most 254 characters.
[[nodiscard]] bool isValidEmail(std::string_view address);

// The whole message (headers and a base64 body), CRLF line ends.
[[nodiscard]] Result<std::string> formatMail(const MailConfig& config, const Mail& mail, std::int64_t unixTime,
                                             std::string_view messageId);

// Sends one message now (blocks until the server answered).
[[nodiscard]] Status sendMail(const MailConfig& config, const Mail& mail);

class Mailer {
public:
    explicit Mailer(MailConfig config);
    ~Mailer();
    Mailer(const Mailer&) = delete;
    Mailer& operator=(const Mailer&) = delete;

    [[nodiscard]] bool enabled() const { return m_config.enabled(); }
    // Queues a message; false if mail is not set up or the queue is full.
    bool send(Mail mail);
    // Waits until the queue is empty (tests and shutting down); false on timeout.
    bool flush(double seconds);

    struct Stats {
        std::size_t queued = 0;
        std::uint64_t sent = 0;
        std::uint64_t failed = 0; // given up after every attempt
    };
    [[nodiscard]] Stats stats() const;

private:
    void run();

    MailConfig m_config;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::deque<Mail> m_queue;
    bool m_busy = false;
    bool m_stop = false;
    Stats m_stats;
    std::thread m_worker;
};

} // namespace px::platform

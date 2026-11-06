#include "Mail.hpp"

#include "px/core/Log.hpp"
#include "px/crypto/Crypto.hpp"
#include "px/crypto/Encoding.hpp"

#if defined(PX_HAVE_TLS)
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#endif

namespace px::platform {

namespace {

bool hasLineBreak(std::string_view s) { return s.find_first_of("\r\n") != std::string_view::npos; }

// The address inside "Name <address>", or the whole text.
std::string bareAddress(std::string_view from) {
    const auto open = from.rfind('<');
    const auto close = from.rfind('>');
    if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
        return std::string(from.substr(open + 1, close - open - 1));
    }
    return std::string(from);
}

// RFC 2047 for a header with non-ASCII text.
std::string headerText(std::string_view text) {
    const bool ascii = std::all_of(text.begin(), text.end(), [](char c) {
        return static_cast<unsigned char>(c) < 128 && c != '=' && c != '?';
    });
    if (ascii) return std::string(text);
    // Standard base64 (not base64url).
    std::string b = crypto::base64UrlEncode(crypto::bytesOf(text));
    for (char& c : b) c = c == '-' ? '+' : c == '_' ? '/' : c;
    while (b.size() % 4 != 0) b += '=';
    return "=?UTF-8?B?" + b + "?=";
}

// "Name <address>" with the name quoted, or encoded when it is not ASCII.
std::string fromHeader(std::string_view from) {
    const auto open = from.rfind('<');
    if (open == std::string_view::npos) return std::string(from);
    std::string name;
    for (const char c : from.substr(0, open)) {
        if (c != '"' && c != '\\') name += c;
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    const std::string address = "<" + bareAddress(from) + ">";
    if (name.empty()) return address;
    const std::string encoded = headerText(name);
    return (encoded.starts_with("=?") ? encoded : "\"" + name + "\"") + " " + address;
}

std::string base64Standard(std::string_view text) {
    std::string b = crypto::base64UrlEncode(crypto::bytesOf(text));
    for (char& c : b) c = c == '-' ? '+' : c == '_' ? '/' : c;
    while (b.size() % 4 != 0) b += '=';
    return b;
}

std::string rfc5322Date(std::int64_t unixTime) {
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return std::format("{}, {:02} {} {} {:02}:{:02}:{:02} +0000", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
                       tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

#if defined(PX_HAVE_TLS)
std::string mbedError(int code) {
    char text[200];
    mbedtls_strerror(code, text, sizeof(text));
    return text;
}

struct Endpoint {
    enum class Security { Implicit, StartTls, None } security = Security::StartTls;
    std::string host;
    std::string port;
};

Result<Endpoint> parseServer(std::string_view url) {
    Endpoint e;
    std::string_view rest;
    if (url.starts_with("smtps://")) {
        e.security = Endpoint::Security::Implicit;
        rest = url.substr(8);
        e.port = "465";
    } else if (url.starts_with("smtp+plain://")) {
        e.security = Endpoint::Security::None;
        rest = url.substr(13);
        e.port = "25";
    } else if (url.starts_with("smtp://")) {
        rest = url.substr(7);
        e.port = "587";
    } else {
        return makeError(ErrorCode::InvalidArgument, "Mail server '{}': use smtps://, smtp:// or smtp+plain://", url);
    }
    while (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
    const auto colon = rest.rfind(':');
    if (colon != std::string_view::npos && rest.find(']') == std::string_view::npos) {
        e.host = std::string(rest.substr(0, colon));
        e.port = std::string(rest.substr(colon + 1));
    } else {
        e.host = std::string(rest);
    }
    if (e.host.empty() || e.port.empty()) return makeError(ErrorCode::InvalidArgument, "Mail server '{}': no host", url);
    return e;
}

// One SMTP conversation over a socket, with TLS once it is started.
class Smtp {
public:
    explicit Smtp(int timeoutMs) : m_timeoutMs(timeoutMs) {
        mbedtls_net_init(&m_net);
        mbedtls_ssl_init(&m_ssl);
        mbedtls_ssl_config_init(&m_conf);
        mbedtls_x509_crt_init(&m_ca);
        mbedtls_ctr_drbg_init(&m_drbg);
        mbedtls_entropy_init(&m_entropy);
    }
    ~Smtp() {
        mbedtls_ssl_free(&m_ssl);
        mbedtls_ssl_config_free(&m_conf);
        mbedtls_x509_crt_free(&m_ca);
        mbedtls_ctr_drbg_free(&m_drbg);
        mbedtls_entropy_free(&m_entropy);
        mbedtls_net_free(&m_net);
    }
    Smtp(const Smtp&) = delete;
    Smtp& operator=(const Smtp&) = delete;

    Status connect(const Endpoint& e) {
        const int r = mbedtls_net_connect(&m_net, e.host.c_str(), e.port.c_str(), MBEDTLS_NET_PROTO_TCP);
        if (r != 0) return makeError(ErrorCode::IoError, "Cannot reach the mail server {}:{} ({})", e.host, e.port, mbedError(r));
        return {};
    }

    Status startTls(const std::string& host, const std::filesystem::path& caFile) {
        static std::once_flag psa;
        std::call_once(psa, [] { (void)psa_crypto_init(); });
        int r = mbedtls_ctr_drbg_seed(&m_drbg, mbedtls_entropy_func, &m_entropy, nullptr, 0);
        if (r != 0) return makeError(ErrorCode::IoError, "TLS: {}", mbedError(r));
        if (auto loaded = loadTrusted(caFile); !loaded) return loaded;
        mbedtls_ssl_config_defaults(&m_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_min_tls_version(&m_conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&m_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&m_conf, &m_ca, nullptr);
        mbedtls_ssl_conf_rng(&m_conf, mbedtls_ctr_drbg_random, &m_drbg);
        mbedtls_ssl_conf_read_timeout(&m_conf, static_cast<std::uint32_t>(m_timeoutMs));
        if ((r = mbedtls_ssl_setup(&m_ssl, &m_conf)) != 0) return makeError(ErrorCode::IoError, "TLS: {}", mbedError(r));
        mbedtls_ssl_set_hostname(&m_ssl, host.c_str());
        mbedtls_ssl_set_bio(&m_ssl, &m_net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
        while ((r = mbedtls_ssl_handshake(&m_ssl)) != 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            const std::uint32_t flags = mbedtls_ssl_get_verify_result(&m_ssl);
            if (flags != 0 && flags != static_cast<std::uint32_t>(-1)) {
                char why[256];
                mbedtls_x509_crt_verify_info(why, sizeof(why), "", flags);
                std::string text = why;
                while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
                return makeError(ErrorCode::IoError, "The mail server's certificate was not accepted: {}", text);
            }
            return makeError(ErrorCode::IoError, "TLS with the mail server failed: {}", mbedError(r));
        }
        m_tls = true;
        m_buffer.clear();
        return {};
    }

    Status write(std::string_view text) {
        const auto* p = reinterpret_cast<const unsigned char*>(text.data());
        std::size_t left = text.size();
        while (left > 0) {
            const int n = m_tls ? mbedtls_ssl_write(&m_ssl, p, left) : mbedtls_net_send(&m_net, p, left);
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (n <= 0) return makeError(ErrorCode::IoError, "The mail server closed the connection ({})", mbedError(n));
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        return {};
    }

    // A reply: the code and the text of its (possibly several) lines.
    struct Reply {
        int code = 0;
        std::string text;
    };

    Result<Reply> reply() {
        Reply r;
        for (int lines = 0; lines < 100; ++lines) {
            auto line = readLine();
            if (!line) return line.error();
            if (line->size() < 3 || !std::isdigit(static_cast<unsigned char>((*line)[0]))) {
                return makeError(ErrorCode::IoError, "The mail server said something unexpected: {}", *line);
            }
            r.code = std::stoi(line->substr(0, 3));
            r.text += line->size() > 4 ? line->substr(4) : std::string();
            r.text += '\n';
            if (line->size() == 3 || (*line)[3] != '-') return r;
        }
        return makeError(ErrorCode::IoError, "The mail server's reply was too long");
    }

    Result<Reply> command(std::string_view line, int expected, std::string_view what) {
        if (auto s = write(std::string(line) + "\r\n"); !s) return s.error();
        auto r = reply();
        if (!r) return r.error();
        if (r->code / 100 != expected / 100) {
            std::string text = r->text;
            while (!text.empty() && text.back() == '\n') text.pop_back();
            return makeError(ErrorCode::IoError, "The mail server refused {}: {} {}", what, r->code, text);
        }
        return r;
    }

private:
    Status loadTrusted(const std::filesystem::path& caFile) {
        if (!caFile.empty()) {
            const int r = mbedtls_x509_crt_parse_file(&m_ca, caFile.string().c_str());
            if (r < 0) return makeError(ErrorCode::IoError, "Cannot read {} ({})", caFile.string(), mbedError(r));
            return {};
        }
        int loaded = 0;
#if defined(_WIN32)
        if (HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT")) {
            PCCERT_CONTEXT c = nullptr;
            while ((c = CertEnumCertificatesInStore(store, c)) != nullptr) {
                if (mbedtls_x509_crt_parse_der(&m_ca, c->pbCertEncoded, c->cbCertEncoded) == 0) ++loaded;
            }
            CertCloseStore(store, 0);
        }
#else
        if (const char* file = std::getenv("SSL_CERT_FILE"); file != nullptr && mbedtls_x509_crt_parse_file(&m_ca, file) >= 0) ++loaded;
        for (const char* file : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                 "/etc/ssl/cert.pem", "/etc/ssl/ca-bundle.pem"}) {
            if (loaded == 0 && mbedtls_x509_crt_parse_file(&m_ca, file) >= 0) ++loaded;
        }
        if (loaded == 0 && mbedtls_x509_crt_parse_path(&m_ca, "/etc/ssl/certs") >= 0) ++loaded;
#endif
        if (loaded == 0) return makeError(ErrorCode::IoError, "No trusted certificates found for the mail server (--smtp-ca)");
        return {};
    }

    Result<std::string> readLine() {
        while (true) {
            const auto end = m_buffer.find("\r\n");
            if (end != std::string::npos) {
                std::string line = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 2);
                return line;
            }
            if (m_buffer.size() > 64 * 1024) return makeError(ErrorCode::IoError, "The mail server sent a very long line");
            unsigned char chunk[2048];
            int n = 0;
            if (m_tls) {
                n = mbedtls_ssl_read(&m_ssl, chunk, sizeof(chunk));
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            } else {
                n = mbedtls_net_recv_timeout(&m_net, chunk, sizeof(chunk), static_cast<std::uint32_t>(m_timeoutMs));
            }
            if (n == MBEDTLS_ERR_SSL_TIMEOUT) return makeError(ErrorCode::IoError, "The mail server did not answer in time");
            if (n <= 0) return makeError(ErrorCode::IoError, "The mail server closed the connection ({})", mbedError(n));
            m_buffer.append(reinterpret_cast<const char*>(chunk), static_cast<std::size_t>(n));
        }
    }

    int m_timeoutMs;
    bool m_tls = false;
    std::string m_buffer;
    mbedtls_net_context m_net;
    mbedtls_ssl_context m_ssl;
    mbedtls_ssl_config m_conf;
    mbedtls_x509_crt m_ca;
    mbedtls_ctr_drbg_context m_drbg;
    mbedtls_entropy_context m_entropy;
};

#endif // PX_HAVE_TLS

std::string domainOf(std::string_view address) {
    const auto at = address.rfind('@');
    return at == std::string_view::npos ? std::string("localhost") : std::string(address.substr(at + 1));
}

} // namespace

bool isValidEmail(std::string_view address) {
    if (address.size() < 3 || address.size() > 254) return false;
    const auto at = address.find('@');
    if (at == std::string_view::npos || at == 0 || at > 64 || address.find('@', at + 1) != std::string_view::npos) return false;
    for (const char c : address) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= ' ' || u == 127 || c == '<' || c == '>' || c == '(' || c == ')' || c == ',' || c == ';' || c == ':' ||
            c == '\\' || c == '"' || c == '[' || c == ']') {
            return false;
        }
    }
    const std::string_view domain = address.substr(at + 1);
    const auto dot = domain.rfind('.');
    return dot != std::string_view::npos && dot > 0 && dot + 2 <= domain.size() && !domain.starts_with('.') &&
           domain.find("..") == std::string_view::npos;
}

Result<std::string> formatMail(const MailConfig& config, const Mail& mail, std::int64_t unixTime,
                               std::string_view messageId) {
    if (!isValidEmail(mail.to)) return makeError(ErrorCode::InvalidArgument, "Not an email address: '{}'", mail.to);
    if (hasLineBreak(mail.subject) || hasLineBreak(config.from) || config.from.empty()) {
        return makeError(ErrorCode::InvalidArgument, "Mail headers cannot have line breaks, and From must be set");
    }
    std::string m;
    m += "From: " + fromHeader(config.from) + "\r\n";
    m += "To: <" + mail.to + ">\r\n";
    m += "Subject: " + headerText(mail.subject) + "\r\n";
    m += "Date: " + rfc5322Date(unixTime) + "\r\n";
    m += std::format("Message-ID: <{}@{}>\r\n", messageId, domainOf(bareAddress(config.from)));
    m += "MIME-Version: 1.0\r\n";
    m += "Content-Type: text/plain; charset=utf-8\r\n";
    m += "Content-Transfer-Encoding: base64\r\n";
    m += "Auto-Submitted: auto-generated\r\n";
    m += "\r\n";
    // Text in its canonical form: lines end with CRLF.
    std::string text;
    text.reserve(mail.text.size() + mail.text.size() / 40);
    for (std::size_t i = 0; i < mail.text.size(); ++i) {
        if (mail.text[i] == '\n' && (i == 0 || mail.text[i - 1] != '\r')) text += '\r';
        text += mail.text[i];
    }
    const std::string body = base64Standard(text);
    for (std::size_t i = 0; i < body.size(); i += 76) m += body.substr(i, 76) + "\r\n";
    return m;
}

Status sendMail(const MailConfig& config, const Mail& mail) {
    std::array<std::uint8_t, 12> idBytes{};
    if (auto s = crypto::randomBytes(idBytes); !s) return s;
    const std::string id = crypto::toHex(idBytes);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto message = formatMail(config, mail, now, id);
    if (!message) return message.error();

    if (!config.outbox.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(config.outbox, ec);
        // Names sort in the order the messages were written.
        static std::atomic<std::uint64_t> sequence{0};
        const auto file = config.outbox / std::format("{}-{:06}-{}.eml", now, sequence.fetch_add(1) % 1000000, id);
        std::ofstream out(file, std::ios::binary);
        out << *message;
        if (!out) return makeError(ErrorCode::IoError, "Cannot write {}", file.string());
        return {};
    }

#if !defined(PX_HAVE_TLS)
    return makeError(ErrorCode::Unsupported, "This build has no TLS, so it cannot send email over SMTP (use --mail-dir)");
#else
    auto endpoint = parseServer(config.server);
    if (!endpoint) return endpoint.error();
    Smtp smtp(static_cast<int>(config.timeoutSeconds * 1000));
    if (auto s = smtp.connect(*endpoint); !s) return s;
    if (endpoint->security == Endpoint::Security::Implicit) {
        if (auto s = smtp.startTls(endpoint->host, config.caFile); !s) return s;
    }
    auto greeting = smtp.reply();
    if (!greeting) return greeting.error();
    if (greeting->code != 220) return makeError(ErrorCode::IoError, "The mail server is not ready: {}", greeting->code);
    const std::string ehlo = "EHLO " + domainOf(bareAddress(config.from));
    auto hello = smtp.command(ehlo, 250, "EHLO");
    if (!hello) return hello.error();
    if (endpoint->security == Endpoint::Security::StartTls) {
        if (hello->text.find("STARTTLS") == std::string::npos) {
            return makeError(ErrorCode::IoError, "The mail server does not offer STARTTLS (use smtps:// or another server)");
        }
        if (auto r = smtp.command("STARTTLS", 220, "STARTTLS"); !r) return r.error();
        if (auto s = smtp.startTls(endpoint->host, config.caFile); !s) return s;
        hello = smtp.command(ehlo, 250, "EHLO");
        if (!hello) return hello.error();
    }
    if (!config.user.empty()) {
        if (endpoint->security == Endpoint::Security::None) {
            return makeError(ErrorCode::InvalidArgument, "Not sending a mail password without encryption (smtp+plain://)");
        }
        std::string plain;
        plain += '\0';
        plain += config.user;
        plain += '\0';
        plain += config.password;
        auto auth = smtp.command("AUTH PLAIN " + base64Standard(plain), 235, "the login");
        crypto::wipe(std::span(reinterpret_cast<std::uint8_t*>(plain.data()), plain.size()));
        if (!auth) return auth.error();
    }
    if (auto r = smtp.command("MAIL FROM:<" + bareAddress(config.from) + ">", 250, "the sender"); !r) return r.error();
    if (auto r = smtp.command("RCPT TO:<" + mail.to + ">", 250, "the recipient"); !r) return r.error();
    if (auto r = smtp.command("DATA", 354, "DATA"); !r) return r.error();
    // Lines starting with a dot get another (none do in base64, but headers might).
    std::string data;
    std::size_t start = 0;
    while (start < message->size()) {
        const auto end = message->find("\r\n", start);
        const std::string_view line = std::string_view(*message).substr(start, end - start);
        if (line.starts_with('.')) data += '.';
        data += line;
        data += "\r\n";
        start = end + 2;
    }
    if (auto s = smtp.write(data); !s) return s;
    if (auto r = smtp.command(".", 250, "the message"); !r) return r.error();
    (void)smtp.command("QUIT", 221, "QUIT");
    return {};
#endif
}

// --- The queue ---------------------------------------------------------------------------------------

Mailer::Mailer(MailConfig config) : m_config(std::move(config)) {
    if (m_config.enabled()) m_worker = std::thread([this] { run(); });
}

Mailer::~Mailer() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

bool Mailer::send(Mail mail) {
    if (!enabled()) return false;
    {
        std::lock_guard lock(m_mutex);
        if (m_queue.size() >= 1000) return false;
        m_queue.push_back(std::move(mail));
        m_stats.queued = m_queue.size();
    }
    m_wake.notify_one();
    return true;
}

bool Mailer::flush(double seconds) {
    std::unique_lock lock(m_mutex);
    return m_idle.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return m_queue.empty() && !m_busy; });
}

Mailer::Stats Mailer::stats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

void Mailer::run() {
    std::unique_lock lock(m_mutex);
    while (true) {
        m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty()) break; // stopping with nothing left
        Mail mail = std::move(m_queue.front());
        m_queue.pop_front();
        m_stats.queued = m_queue.size();
        m_busy = true;
        lock.unlock();
        double wait = m_config.retrySeconds;
        Status sent = makeError(ErrorCode::IoError, "not tried");
        for (int attempt = 1; attempt <= std::max(1, m_config.attempts); ++attempt) {
            sent = sendMail(m_config, mail);
            if (sent) break;
            log::warn("Mail to {} (attempt {}): {}", mail.to, attempt, sent.error().message);
            if (attempt == m_config.attempts) break;
            std::unique_lock pause(m_mutex);
            if (m_wake.wait_for(pause, std::chrono::duration<double>(wait), [this] { return m_stop; })) break;
            wait *= 2;
        }
        lock.lock();
        if (sent) {
            ++m_stats.sent;
            log::info("Mail sent to {}: {}", mail.to, mail.subject);
        } else {
            ++m_stats.failed;
            log::error("Mail to {} was not sent: {}", mail.to, sent.error().message);
        }
        m_busy = false;
        m_idle.notify_all();
        if (m_stop && m_queue.empty()) break;
    }
    m_busy = false;
    m_idle.notify_all();
}

} // namespace px::platform

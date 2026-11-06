// Accounts beyond logging in (docs/accounts.md): email addresses confirmed
// by a link, password resets and changes, deleting an account, downloading
// its data, and the mail that carries the links (SMTP with TLS, or files).
#include "Mail.hpp"
#include "PlatformHarness.hpp"
#include "px/crypto/Encoding.hpp"

#include <doctest.h>
#include <nlohmann/json.hpp>

#if defined(PX_HAVE_TLS)
#include "Certificates.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif

#include <fstream>
#include <random>
#include <regex>
#include <thread>

using namespace px;
using namespace px::online;
using pxtest::contains;
using pxtest::TestPlatform;

namespace {

std::string decodeBase64(std::string text) {
    std::string url;
    for (const char c : text) {
        if (c == '+') url += '-';
        else if (c == '/') url += '_';
        else if (c != '=' && c != '\r' && c != '\n') url += c;
    }
    auto bytes = crypto::base64UrlDecode(url);
    REQUIRE(bytes.has_value());
    return std::string(bytes->begin(), bytes->end());
}

struct ReadMail {
    std::string headers;
    std::string text;
};

// A message as formatMail writes it: headers, a blank line, base64.
ReadMail parseMail(const std::string& message) {
    const auto split = message.find("\r\n\r\n");
    REQUIRE(split != std::string::npos);
    std::string text = decodeBase64(message.substr(split + 4));
    CHECK(std::regex_search(text, std::regex("[^\r]\n")) == false); // canonical line ends
    for (std::size_t at; (at = text.find("\r\n")) != std::string::npos;) text.erase(at, 1);
    return {message.substr(0, split + 2), text};
}

// The messages in a --mail-dir folder, oldest first.
std::vector<ReadMail> outbox(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::vector<ReadMail> mails;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        mails.push_back(parseMail(std::string(std::istreambuf_iterator<char>(in), {})));
    }
    return mails;
}

// The token in a "…/play#verify=TOKEN" (or reset) link of a message.
std::string linkToken(const ReadMail& mail, const char* kind) {
    std::smatch m;
    const std::regex re(std::string("/play#") + kind + "=([A-Za-z0-9_-]+)");
    REQUIRE(std::regex_search(mail.text, m, re));
    return m[1].str();
}

struct MailPlatform : TestPlatform {
    std::filesystem::path mailDir;
    explicit MailPlatform(bool requireVerified = false)
        : TestPlatform(false, 0, [requireVerified](platform::ServiceConfig& c) {
              c.mail.outbox = c.dataDir / "outbox";
              c.mail.from = "Pixel Test <noreply@example.com>";
              c.requireVerifiedEmail = requireVerified;
          }),
          mailDir(dir / "outbox") {}
    std::vector<ReadMail> mails() {
        REQUIRE(service->mailer().flush(10));
        return outbox(mailDir);
    }
};

} // namespace

TEST_CASE("Mail: addresses and the message format") {
    CHECK(platform::isValidEmail("ada@example.com"));
    CHECK(platform::isValidEmail("ada.lovelace+pixel@mail.example.co.uk"));
    CHECK_FALSE(platform::isValidEmail(""));
    CHECK_FALSE(platform::isValidEmail("ada"));
    CHECK_FALSE(platform::isValidEmail("ada@example"));
    CHECK_FALSE(platform::isValidEmail("@example.com"));
    CHECK_FALSE(platform::isValidEmail("ada@@example.com"));
    CHECK_FALSE(platform::isValidEmail("ada@example..com"));
    CHECK_FALSE(platform::isValidEmail("ada lovelace@example.com"));
    CHECK_FALSE(platform::isValidEmail("ada@example.com\r\nBcc: eve@example.com"));
    CHECK_FALSE(platform::isValidEmail("<ada@example.com>"));
    CHECK_FALSE(platform::isValidEmail(std::string(250, 'a') + "@example.com"));

    platform::MailConfig config;
    config.from = "Pixel, the platform <noreply@example.com>";
    platform::Mail mail{"ada@example.com", "Réinitialiser", "Hello Ada,\n\nLine two: ünïcode.\n.\nThe end."};
    auto message = platform::formatMail(config, mail, 1'700'000'000, "abc123");
    REQUIRE(message.ok());
    const ReadMail read = parseMail(message.value());
    CHECK(contains(read.headers, "From: \"Pixel, the platform\" <noreply@example.com>\r\n"));
    CHECK(contains(read.headers, "To: <ada@example.com>\r\n"));
    CHECK(contains(read.headers, "Subject: =?UTF-8?B?"));
    CHECK(contains(read.headers, "Date: Tue, 14 Nov 2023 22:13:20 +0000\r\n"));
    CHECK(contains(read.headers, "Message-ID: <abc123@example.com>\r\n"));
    CHECK(contains(read.headers, "Content-Transfer-Encoding: base64\r\n"));
    CHECK(contains(read.headers, "Auto-Submitted: auto-generated\r\n"));
    CHECK(read.text == mail.text);
    std::size_t longest = 0;
    for (std::size_t start = 0, end; (end = message->find("\r\n", start)) != std::string::npos; start = end + 2) {
        longest = std::max(longest, end - start);
    }
    CHECK(longest <= 78); // short lines
    CHECK(message->ends_with("\r\n"));

    // Nothing can add headers.
    CHECK_FALSE(platform::formatMail(config, {"ada@example.com\r\nBcc: eve@example.com", "Hi", "x"}, 0, "id").ok());
    CHECK_FALSE(platform::formatMail(config, {"ada@example.com", "Hi\r\nBcc: eve@example.com", "x"}, 0, "id").ok());
    config.from = "noreply@example.com\nBcc: eve@example.com";
    CHECK_FALSE(platform::formatMail(config, mail, 0, "id").ok());
}

TEST_CASE("Accounts: an email address confirmed by a link") {
    MailPlatform p;
    auto info = p.client().info();
    REQUIRE(info.ok());
    CHECK(info->mail);
    auto ada = p.account("Ada");
    auto settings = ada.account();
    REQUIRE(settings.ok());
    CHECK(settings->email.empty());
    CHECK(settings->mail);
    CHECK_FALSE(settings->emailVerified);
    CHECK_FALSE(settings->emailRequiredToPublish);

    CHECK(contains(ada.setEmail("not an address", "password-Ada").error().message, "does not look like"));
    CHECK(ada.setEmail("ada@example.com", "wrong password").error().code == ErrorCode::AccessDenied);
    CHECK(p.mails().empty());
    REQUIRE(ada.setEmail("Ada@Example.com", "password-Ada").ok());
    settings = ada.account();
    CHECK(settings->email == "ada@example.com"); // kept in lower case
    CHECK_FALSE(settings->emailVerified);

    auto mails = p.mails();
    REQUIRE(mails.size() == 1);
    CHECK(contains(mails[0].headers, "To: <ada@example.com>"));
    CHECK(contains(mails[0].headers, "From: \"Pixel Test\" <noreply@example.com>"));
    CHECK(contains(mails[0].text, "Hello Ada"));
    CHECK(contains(mails[0].text, p.service->url() + "/play#verify="));
    const std::string token = linkToken(mails[0], "verify");

    // Anyone with the link confirms it (no login needed), once.
    CHECK_FALSE(p.client().verifyEmail("made-up").ok());
    auto verified = p.client().verifyEmail(token);
    REQUIRE_MESSAGE(verified.ok(), (verified.ok() ? "" : verified.error().message));
    CHECK(verified.value() == "Ada");
    CHECK(ada.account()->emailVerified);
    CHECK(contains(p.client().verifyEmail(token).error().message, "used already"));
    CHECK(contains(ada.resendVerification().error().message, "confirmed already"));

    // A new address needs confirming again; the old link would not do it.
    REQUIRE(ada.setEmail("ada@example.org", "password-Ada").ok());
    CHECK_FALSE(ada.account()->emailVerified);
    REQUIRE(ada.resendVerification().ok());
    mails = p.mails();
    REQUIRE(mails.size() == 3);
    const std::string first = linkToken(mails[1], "verify");
    const std::string second = linkToken(mails[2], "verify");
    CHECK(first != second);
    CHECK_FALSE(p.client().verifyEmail(first).ok()); // replaced by the second
    REQUIRE(p.client().verifyEmail(second).ok());
    CHECK(ada.account()->email == "ada@example.org");
    CHECK(ada.account()->emailVerified);

    // Removing the address.
    REQUIRE(ada.setEmail("", "password-Ada").ok());
    CHECK(ada.account()->email.empty());
    CHECK(p.mails().size() == 3);
    CHECK(contains(ada.resendVerification().error().message, "Add an email address"));
    CHECK_FALSE(p.client().account().ok());
}

TEST_CASE("Accounts: password resets by email, changes and admin links") {
    MailPlatform p;
    auto ada = p.account("Ada");
    auto adaPhone = p.client();
    REQUIRE(adaPhone.login("Ada", "password-Ada").ok());
    auto bob = p.account("Bob");

    // Only confirmed addresses get links; the answer never says who exists.
    REQUIRE(ada.setEmail("ada@example.com", "password-Ada").ok());
    REQUIRE(bob.setEmail("bob@example.com", "password-Bob").ok());
    auto mails = p.mails();
    REQUIRE(mails.size() == 2);
    REQUIRE(p.client().verifyEmail(linkToken(mails[0], "verify")).ok()); // Ada's
    CHECK(p.client().requestPasswordReset("Bob").ok());     // not confirmed: no mail
    CHECK(p.client().requestPasswordReset("nobody").ok());  // no such account: no mail
    CHECK(p.client().requestPasswordReset("eve@example.com").ok());
    CHECK(p.mails().size() == 2);
    CHECK(p.client().requestPasswordReset("ADA@example.com").ok()); // by address
    CHECK(p.client().requestPasswordReset("ada").ok());             // by name
    mails = p.mails();
    REQUIRE(mails.size() == 4);
    CHECK(contains(mails[3].headers, "To: <ada@example.com>"));
    CHECK(contains(mails[3].text, "reset the password of the account Ada"));
    const std::string stale = linkToken(mails[2], "reset");
    const std::string token = linkToken(mails[3], "reset");
    CHECK_FALSE(p.client().completePasswordReset(stale, "a new password").ok()); // the newer link replaced it

    CHECK(contains(p.client().completePasswordReset(token, "short").error().message, "at least 8"));
    auto reset = p.client().completePasswordReset(token, "a new password");
    REQUIRE_MESSAGE(reset.ok(), (reset.ok() ? "" : reset.error().message));
    CHECK(reset.value() == "Ada");
    CHECK_FALSE(p.client().completePasswordReset(token, "another one").ok()); // once
    // Everyone who was logged in is out; only the new password works.
    CHECK(ada.me().error().code == ErrorCode::AccessDenied);
    CHECK_FALSE(adaPhone.me().ok());
    CHECK_FALSE(p.client().login("Ada", "password-Ada").ok());
    auto again = p.client();
    REQUIRE(again.login("Ada", "a new password").ok());
    auto other = p.client();
    REQUIRE(other.login("Ada", "a new password").ok());

    // Changing it: the current one is needed; other sessions end, this one stays.
    CHECK(again.changePassword("wrong", "third password").error().code == ErrorCode::AccessDenied);
    CHECK(contains(again.changePassword("a new password", "short").error().message, "at least 8"));
    REQUIRE(again.changePassword("a new password", "third password").ok());
    CHECK(again.me().ok());
    CHECK_FALSE(other.me().ok());
    CHECK(p.client().login("Ada", "third password").ok());

    // The audit log has the reset.
    REQUIRE(p.service->setRole("Bob", Role::Admin).ok());
    auto log = bob.audit();
    REQUIRE(log.ok());
    CHECK(std::any_of(log->begin(), log->end(), [](const AuditEntry& e) { return e.action == "password_reset"; }));

    // An operator can make a link for someone who has no email address.
    auto carol = p.account("Carol");
    auto link = p.service->passwordResetLink("carol");
    REQUIRE(link.ok());
    CHECK(link->starts_with(p.service->url() + "/play#reset="));
    const std::string carolToken = link->substr(link->find('=') + 1);
    REQUIRE(p.client().completePasswordReset(carolToken, "carol's new one").ok());
    CHECK(p.client().login("Carol", "carol's new one").ok());
    CHECK_FALSE(p.service->passwordResetLink("nobody").ok());
}

TEST_CASE("Accounts: without email, and publishing that needs a confirmed address") {
    {
        TestPlatform p; // no mail set up
        CHECK_FALSE(p.client().info()->mail);
        auto ada = p.account("Ada");
        CHECK_FALSE(ada.account()->mail);
        CHECK(contains(ada.setEmail("ada@example.com", "password-Ada").error().message, "cannot send email"));
        CHECK(contains(p.client().requestPasswordReset("Ada").error().message, "cannot send email"));
        CHECK(ada.changePassword("password-Ada", "something new").ok()); // still works
    }
    MailPlatform p(true);
    auto ada = p.account("Ada");
    CHECK(ada.account()->emailRequiredToPublish);
    auto refused = ada.createGame("Coin Run", "", Visibility::Public);
    REQUIRE_FALSE(refused.ok());
    CHECK(contains(refused.error().message, "Confirm your email address"));
    REQUIRE(ada.setEmail("ada@example.com", "password-Ada").ok());
    REQUIRE(p.client().verifyEmail(linkToken(p.mails().at(0), "verify")).ok());
    auto game = ada.createGame("Coin Run", "", Visibility::Public);
    REQUIRE(game.ok());
    CHECK(ada.publish(game->id, pxtest::examplePackage("platformer"), "first").ok());
}

TEST_CASE("Accounts: downloading your data and deleting the account") {
    MailPlatform p;
    auto ada = p.account("Ada");
    auto bob = p.account("Bob");
    REQUIRE(ada.setEmail("ada@example.com", "password-Ada").ok());
    ProfileUpdate profile;
    profile.bio = "I make games";
    REQUIRE(ada.updateProfile(profile).ok());
    REQUIRE(ada.requestFriend("Bob").ok());
    REQUIRE(bob.acceptFriend("Ada").ok());
    const auto package = pxtest::examplePackage("platformer");
    const auto other = pxtest::examplePackage("topdown");
    auto shared = ada.createGame("Coin Run", "Collect every coin.", Visibility::Public);
    auto own = ada.createGame("Maze", "", Visibility::Public);
    auto bobs = bob.createGame("Bob's Coin Run", "", Visibility::Public);
    REQUIRE((shared.ok() && own.ok() && bobs.ok()));
    auto v1 = ada.publish(shared->id, package, "first");
    auto v2 = ada.publish(own->id, other, "first");
    REQUIRE((v1.ok() && v2.ok() && bob.publish(bobs->id, package, "same").ok()));

    auto exported = ada.exportData();
    REQUIRE(exported.ok());
    const auto data = nlohmann::json::parse(exported.value());
    CHECK(data["account"]["username"] == "Ada");
    CHECK(data["account"]["email"] == "ada@example.com");
    CHECK(data["account"]["bio"] == "I make games");
    CHECK(data["friends"].size() == 1);
    CHECK(data["friends"][0]["username"] == "Bob");
    CHECK(data["games"].size() == 2);
    CHECK(data["sessions"].size() == 1);
    CHECK_FALSE(contains(exported.value(), "password"));
    CHECK_FALSE(p.client().exportData().ok());

    CHECK(ada.deleteAccount("wrong").error().code == ErrorCode::AccessDenied);
    REQUIRE(ada.deleteAccount("password-Ada").ok());
    CHECK(ada.token().empty());
    CHECK_FALSE(p.client().login("Ada", "password-Ada").ok());
    CHECK_FALSE(p.client().profile("Ada").ok());
    CHECK_FALSE(p.client().game(shared->id).ok());
    CHECK_FALSE(p.client().game(own->id).ok());
    CHECK(p.client().game(bobs->id).ok());
    CHECK(bob.social()->friends.empty());
    CHECK(p.client().searchUsers("Ad")->empty());
    auto tombstones = p.client().searchUsers("deleted");
    REQUIRE(tombstones.ok());
    CHECK(tombstones->empty());
    // Package files nothing else uses are gone; Bob's (the same file) stays.
    const auto packageFile = [&](const std::string& hash) {
        return p.dir / "packages" / hash.substr(0, 2) / (hash + ".pxpkg");
    };
    CHECK(std::filesystem::exists(packageFile(v1->packageHash)));
    CHECK_FALSE(std::filesystem::exists(packageFile(v2->packageHash)));
    CHECK(bob.downloadPackage(v1->packageHash).ok());
    // The name is free again.
    CHECK(p.client().registerAccount("Ada", "a whole new Ada").ok());

    // Administrators keep their account until the role is gone.
    REQUIRE(p.service->setRole("Bob", Role::Admin).ok());
    CHECK(contains(bob.deleteAccount("password-Bob").error().message, "Administrators"));
    auto log = bob.audit();
    REQUIRE(log.ok());
    CHECK(std::any_of(log->begin(), log->end(), [](const AuditEntry& e) { return e.action == "delete_account"; }));
}

TEST_CASE("Mail: a queue that gives up after its attempts") {
    platform::MailConfig config;
    config.server = "smtp://127.0.0.1:9"; // nothing listens on the discard port
    config.from = "noreply@example.com";
    config.attempts = 2;
    config.retrySeconds = 0.05;
    config.timeoutSeconds = 2;
    platform::Mailer mailer(config);
    REQUIRE(mailer.send({"ada@example.com", "Hi", "Hello"}));
    REQUIRE(mailer.flush(30));
    CHECK(mailer.stats().failed == 1);
    CHECK(mailer.stats().sent == 0);
    platform::Mailer off(platform::MailConfig{});
    CHECK_FALSE(off.send({"ada@example.com", "Hi", "Hello"}));
}

#if defined(PX_HAVE_TLS)
namespace {

// Just enough of an SMTP server for one message: STARTTLS (or TLS from the
// start) with a development certificate, AUTH, MAIL, RCPT, DATA.
class FakeSmtp {
public:
    enum class Mode { StartTls, Implicit, NoStartTls, Plain };

    FakeSmtp(Mode mode, const std::filesystem::path& cert, const std::filesystem::path& key) : m_mode(mode) {
        (void)psa_crypto_init();
        mbedtls_net_init(&m_listen);
        mbedtls_net_init(&m_client);
        mbedtls_ssl_init(&m_ssl);
        mbedtls_ssl_config_init(&m_conf);
        mbedtls_x509_crt_init(&m_cert);
        mbedtls_pk_init(&m_key);
        mbedtls_entropy_init(&m_entropy);
        mbedtls_ctr_drbg_init(&m_drbg);
        REQUIRE(mbedtls_ctr_drbg_seed(&m_drbg, mbedtls_entropy_func, &m_entropy, nullptr, 0) == 0);
        REQUIRE(mbedtls_x509_crt_parse_file(&m_cert, cert.string().c_str()) == 0);
        REQUIRE(mbedtls_pk_parse_keyfile(&m_key, key.string().c_str(), nullptr, mbedtls_ctr_drbg_random, &m_drbg) == 0);
        std::mt19937 random(std::random_device{}());
        for (int tries = 0; tries < 50; ++tries) {
            port = 31000 + static_cast<int>(random() % 4000);
            if (mbedtls_net_bind(&m_listen, "127.0.0.1", std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP) == 0) break;
            port = 0;
        }
        REQUIRE(port != 0);
        m_thread = std::thread([this] { serve(); });
    }
    ~FakeSmtp() {
        wait();
        mbedtls_ssl_free(&m_ssl);
        mbedtls_ssl_config_free(&m_conf);
        mbedtls_x509_crt_free(&m_cert);
        mbedtls_pk_free(&m_key);
        mbedtls_ctr_drbg_free(&m_drbg);
        mbedtls_entropy_free(&m_entropy);
        mbedtls_net_free(&m_client);
        mbedtls_net_free(&m_listen);
    }

    int port = 0;
    std::vector<std::string> commands; // read these after wait()
    std::string data;
    bool tlsUsed = false;

    void wait() {
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void serve() {
        if (mbedtls_net_poll(&m_listen, MBEDTLS_NET_POLL_READ, 15000) <= 0) return;
        if (mbedtls_net_accept(&m_listen, &m_client, nullptr, 0, nullptr) != 0) return;
        if (m_mode == Mode::Implicit && !startTls()) return;
        if (!write("220 fake.example ESMTP\r\n")) return;
        while (true) {
            std::string line;
            if (!readLine(line)) return;
            commands.push_back(line);
            if (line.starts_with("EHLO")) {
                const bool offer = m_mode == Mode::StartTls && !m_tls;
                if (!write(offer ? "250-fake.example\r\n250-STARTTLS\r\n250 AUTH PLAIN\r\n" : "250-fake.example\r\n250 AUTH PLAIN\r\n")) {
                    return;
                }
            } else if (line == "STARTTLS") {
                if (!write("220 go ahead\r\n") || !startTls()) return;
            } else if (line.starts_with("AUTH PLAIN ")) {
                write("235 ok\r\n");
            } else if (line.starts_with("MAIL FROM:") || line.starts_with("RCPT TO:")) {
                write("250 ok\r\n");
            } else if (line == "DATA") {
                write("354 go on\r\n");
                while (readLine(line) && line != ".") data += line + "\r\n";
                write("250 queued\r\n");
            } else if (line == "QUIT") {
                write("221 bye\r\n");
                return;
            } else {
                write("500 what\r\n");
            }
        }
    }

    bool startTls() {
        if (mbedtls_ssl_config_defaults(&m_conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            return false;
        }
        mbedtls_ssl_conf_rng(&m_conf, mbedtls_ctr_drbg_random, &m_drbg);
        mbedtls_ssl_conf_read_timeout(&m_conf, 10000);
        if (mbedtls_ssl_conf_own_cert(&m_conf, &m_cert, &m_key) != 0) return false;
        if (mbedtls_ssl_setup(&m_ssl, &m_conf) != 0) return false;
        mbedtls_ssl_set_bio(&m_ssl, &m_client, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
        int r = 0;
        while ((r = mbedtls_ssl_handshake(&m_ssl)) != 0) {
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
        }
        m_tls = true;
        tlsUsed = true;
        return true;
    }

    bool write(std::string_view text) {
        const auto* p = reinterpret_cast<const unsigned char*>(text.data());
        std::size_t left = text.size();
        while (left > 0) {
            const int n = m_tls ? mbedtls_ssl_write(&m_ssl, p, left) : mbedtls_net_send(&m_client, p, left);
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) continue;
            if (n <= 0) return false;
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool readLine(std::string& line) {
        while (true) {
            if (const auto end = m_buffer.find("\r\n"); end != std::string::npos) {
                line = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 2);
                return true;
            }
            unsigned char chunk[1024];
            const int n = m_tls ? mbedtls_ssl_read(&m_ssl, chunk, sizeof(chunk))
                                : mbedtls_net_recv_timeout(&m_client, chunk, sizeof(chunk), 10000);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) return false;
            m_buffer.append(reinterpret_cast<const char*>(chunk), static_cast<std::size_t>(n));
        }
    }

    Mode m_mode;
    bool m_tls = false;
    std::string m_buffer;
    std::thread m_thread;
    mbedtls_net_context m_listen;
    mbedtls_net_context m_client;
    mbedtls_ssl_context m_ssl;
    mbedtls_ssl_config m_conf;
    mbedtls_x509_crt m_cert;
    mbedtls_pk_context m_key;
    mbedtls_entropy_context m_entropy;
    mbedtls_ctr_drbg_context m_drbg;
};

} // namespace

TEST_CASE("Mail: SMTP with STARTTLS, TLS from the start, and the refusals") {
    const auto certDir = pxtest::tempDir("smtp");
    const auto cert = certDir / "cert.pem";
    const auto key = certDir / "key.pem";
    REQUIRE(platform::makeDevCertificate({"localhost", "127.0.0.1"}, cert, key).ok());
    const platform::Mail mail{"ada@example.com", "Confirm your address", "Hello Ada,\n\nhttps://example.com/play#verify=abc\n"};

    platform::MailConfig config;
    config.from = "Pixel <noreply@example.com>";
    config.user = "pixel";
    config.password = "smtp secret";
    config.caFile = cert;
    config.timeoutSeconds = 10;

    SUBCASE("STARTTLS, then the login and the message") {
        FakeSmtp server(FakeSmtp::Mode::StartTls, cert, key);
        config.server = std::format("smtp://localhost:{}", server.port);
        auto sent = platform::sendMail(config, mail);
        server.wait();
        REQUIRE_MESSAGE(sent.ok(), (sent.ok() ? "" : sent.error().message));
        CHECK(server.tlsUsed);
        REQUIRE(server.commands.size() == 8);
        CHECK(server.commands[0] == "EHLO example.com");
        CHECK(server.commands[1] == "STARTTLS");
        CHECK(server.commands[2] == "EHLO example.com");
        std::string plain;
        plain += '\0';
        plain += "pixel";
        plain += '\0';
        plain += "smtp secret";
        CHECK(server.commands[3] == "AUTH PLAIN " + [&] {
            std::string b = crypto::base64UrlEncode(crypto::bytesOf(plain));
            for (char& c : b) c = c == '-' ? '+' : c == '_' ? '/' : c;
            while (b.size() % 4 != 0) b += '=';
            return b;
        }());
        CHECK(server.commands[4] == "MAIL FROM:<noreply@example.com>");
        CHECK(server.commands[5] == "RCPT TO:<ada@example.com>");
        CHECK(server.commands[6] == "DATA");
        CHECK(server.commands[7] == "QUIT");
        const ReadMail read = parseMail(server.data);
        CHECK(contains(read.headers, "To: <ada@example.com>"));
        CHECK(read.text == mail.text);
    }
    SUBCASE("TLS from the start (smtps://)") {
        FakeSmtp server(FakeSmtp::Mode::Implicit, cert, key);
        config.server = std::format("smtps://localhost:{}", server.port);
        auto sent = platform::sendMail(config, mail);
        server.wait();
        REQUIRE_MESSAGE(sent.ok(), (sent.ok() ? "" : sent.error().message));
        CHECK(server.tlsUsed);
        CHECK(server.commands.at(0) == "EHLO example.com");
        CHECK(server.commands.at(1).starts_with("AUTH PLAIN "));
    }
    SUBCASE("a server without STARTTLS never sees the password") {
        FakeSmtp server(FakeSmtp::Mode::NoStartTls, cert, key);
        config.server = std::format("smtp://localhost:{}", server.port);
        auto sent = platform::sendMail(config, mail);
        server.wait();
        REQUIRE_FALSE(sent.ok());
        CHECK(contains(sent.error().message, "does not offer STARTTLS"));
        CHECK(server.commands == std::vector<std::string>{"EHLO example.com"});
    }
    SUBCASE("a certificate this computer does not trust") {
        FakeSmtp server(FakeSmtp::Mode::Implicit, cert, key);
        config.server = std::format("smtps://localhost:{}", server.port);
        config.caFile.clear(); // the system's trusted roots
        auto sent = platform::sendMail(config, mail);
        server.wait();
        REQUIRE_FALSE(sent.ok());
        CHECK(contains(sent.error().message, "certificate"));
        CHECK(server.commands.empty());
    }
    SUBCASE("no password over an unencrypted relay") {
        FakeSmtp server(FakeSmtp::Mode::Plain, cert, key);
        config.server = std::format("smtp+plain://127.0.0.1:{}", server.port);
        auto refused = platform::sendMail(config, mail);
        REQUIRE_FALSE(refused.ok());
        CHECK(contains(refused.error().message, "without encryption"));
        server.wait();
        for (const auto& c : server.commands) CHECK_FALSE(c.starts_with("AUTH"));
    }
    SUBCASE("an unencrypted relay without a login") {
        FakeSmtp server(FakeSmtp::Mode::Plain, cert, key);
        config.server = std::format("smtp+plain://127.0.0.1:{}", server.port);
        config.user.clear();
        auto sent = platform::sendMail(config, mail);
        server.wait();
        REQUIRE_MESSAGE(sent.ok(), (sent.ok() ? "" : sent.error().message));
        CHECK_FALSE(server.tlsUsed);
        CHECK(server.commands.size() == 5);
    }
    std::filesystem::remove_all(certDir);
}
#endif

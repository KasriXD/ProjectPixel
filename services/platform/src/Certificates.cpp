#include "Certificates.hpp"

#if defined(PX_HAVE_TLS)
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>

namespace px::platform {

#if defined(PX_HAVE_TLS)
namespace {
struct Rng {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    Rng() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Rng() {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
};

std::string utcStamp(std::chrono::system_clock::time_point t) {
    const std::time_t time = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    return std::format("{:04}{:02}{:02}{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                       tm.tm_min, tm.tm_sec);
}

bool parseIpv4(const std::string& text, std::array<unsigned char, 4>& out) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = {static_cast<unsigned char>(a), static_cast<unsigned char>(b), static_cast<unsigned char>(c),
           static_cast<unsigned char>(d)};
    return true;
}

Error tlsError(const char* what, int code) {
    return Error{ErrorCode::Unknown, std::format("Making the certificate failed ({}: -0x{:04x})", what, -code)};
}
} // namespace

Status makeDevCertificate(const std::vector<std::string>& hosts, const std::filesystem::path& certFile,
                          const std::filesystem::path& keyFile) {
    if (hosts.empty()) return makeError(ErrorCode::InvalidArgument, "Name at least one host for the certificate");
    Rng rng;
    const char* personal = "px-platform dev certificate";
    if (int rc = mbedtls_ctr_drbg_seed(&rng.drbg, mbedtls_entropy_func, &rng.entropy,
                                       reinterpret_cast<const unsigned char*>(personal), std::strlen(personal));
        rc != 0) {
        return tlsError("random", rc);
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    std::unique_ptr<mbedtls_pk_context, void (*)(mbedtls_pk_context*)> keyGuard(&key, mbedtls_pk_free);
    if (int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)); rc != 0) return tlsError("key", rc);
    if (int rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &rng.drbg);
        rc != 0) {
        return tlsError("key", rc);
    }

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    std::unique_ptr<mbedtls_x509write_cert, void (*)(mbedtls_x509write_cert*)> crtGuard(&crt, mbedtls_x509write_crt_free);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    const std::string name = std::format("CN={},O=Project Pixel development", hosts.front());
    if (int rc = mbedtls_x509write_crt_set_subject_name(&crt, name.c_str()); rc != 0) return tlsError("name", rc);
    if (int rc = mbedtls_x509write_crt_set_issuer_name(&crt, name.c_str()); rc != 0) return tlsError("name", rc);
    std::array<unsigned char, 16> serial{};
    mbedtls_ctr_drbg_random(&rng.drbg, serial.data(), serial.size());
    serial[0] &= 0x7F; // positive
    serial[0] |= 0x01;
    if (int rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial.data(), serial.size()); rc != 0) {
        return tlsError("serial", rc);
    }
    const auto now = std::chrono::system_clock::now();
    const std::string from = utcStamp(now - std::chrono::hours(24));
    const std::string until = utcStamp(now + std::chrono::hours(24 * 365));
    if (int rc = mbedtls_x509write_crt_set_validity(&crt, from.c_str(), until.c_str()); rc != 0) {
        return tlsError("validity", rc);
    }
    mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_AGREEMENT);

    // Subject alternative names: what clients check the host name against.
    std::vector<mbedtls_x509_san_list> sans(hosts.size());
    std::vector<std::array<unsigned char, 4>> ips(hosts.size());
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        mbedtls_x509_san_list& n = sans[i];
        n.next = i + 1 < hosts.size() ? &sans[i + 1] : nullptr;
        if (parseIpv4(hosts[i], ips[i])) {
            n.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
            n.node.san.unstructured_name.p = ips[i].data();
            n.node.san.unstructured_name.len = 4;
        } else {
            n.node.type = MBEDTLS_X509_SAN_DNS_NAME;
            n.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(const_cast<char*>(hosts[i].data()));
            n.node.san.unstructured_name.len = hosts[i].size();
        }
        n.node.san.unstructured_name.tag = 0;
    }
    if (int rc = mbedtls_x509write_crt_set_subject_alternative_name(&crt, sans.data()); rc != 0) {
        return tlsError("names", rc);
    }

    std::vector<unsigned char> certPem(8192);
    if (int rc = mbedtls_x509write_crt_pem(&crt, certPem.data(), certPem.size(), mbedtls_ctr_drbg_random, &rng.drbg);
        rc != 0) {
        return tlsError("certificate", rc);
    }
    std::vector<unsigned char> keyPem(4096);
    if (int rc = mbedtls_pk_write_key_pem(&key, keyPem.data(), keyPem.size()); rc != 0) return tlsError("key file", rc);

    std::error_code ec;
    if (certFile.has_parent_path()) std::filesystem::create_directories(certFile.parent_path(), ec);
    if (keyFile.has_parent_path()) std::filesystem::create_directories(keyFile.parent_path(), ec);
    {
        std::ofstream create(keyFile, std::ios::trunc);
    }
    std::filesystem::permissions(keyFile, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);
    std::ofstream keyOut(keyFile, std::ios::trunc);
    keyOut << reinterpret_cast<const char*>(keyPem.data());
    std::ofstream certOut(certFile, std::ios::trunc);
    certOut << reinterpret_cast<const char*>(certPem.data());
    if (!keyOut || !certOut) return makeError(ErrorCode::IoError, "Cannot write {}", certFile.string());
    return {};
}
#else
Status makeDevCertificate(const std::vector<std::string>&, const std::filesystem::path&, const std::filesystem::path&) {
    return makeError(ErrorCode::Unsupported, "This build has no TLS support");
}
#endif

} // namespace px::platform

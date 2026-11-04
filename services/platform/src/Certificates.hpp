#pragma once

#include "px/core/Result.hpp"

#include <filesystem>
#include <string>
#include <vector>

// A self-signed TLS certificate for trying HTTPS on a development machine or
// a home network (docs/platform.md#https). Clients trust it only when told
// to (PX_PLATFORM_CA); real deployments use a certificate from a CA.
namespace px::platform {

// Writes an ECDSA P-256 key and a certificate valid for `hosts` (names or
// IPv4 addresses) for one year, both PEM. The key file is readable only by
// the owner.
[[nodiscard]] Status makeDevCertificate(const std::vector<std::string>& hosts, const std::filesystem::path& certFile,
                                        const std::filesystem::path& keyFile);

} // namespace px::platform

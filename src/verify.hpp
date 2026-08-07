#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace updater {

/// Ed25519 verification of `signature` (raw 64 bytes) over `message`, using
/// the public key embedded at build time (src/public_key.hpp).
[[nodiscard]] bool verifyManifestSignature(std::string_view message, std::string_view signature);

/// Strict base64 decode (standard alphabet, required padding). The error
/// names what was malformed so signature failures are diagnosable.
[[nodiscard]] std::expected<std::string, std::string> base64Decode(std::string_view text);

}  // namespace updater

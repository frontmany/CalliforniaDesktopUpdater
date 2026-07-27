#pragma once

#include <string>
#include <string_view>

namespace updater {

/// Ed25519 verification of `signature` (raw 64 bytes) over `message`, using
/// the public key embedded at build time (src/public_key.hpp).
[[nodiscard]] bool verifyManifestSignature(std::string_view message, std::string_view signature);

/// Strict base64 decode (standard alphabet, required padding). Returns false
/// on any malformed input.
[[nodiscard]] bool base64Decode(std::string_view text, std::string& out);

}

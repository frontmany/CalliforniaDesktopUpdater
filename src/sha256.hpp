#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace updater {

/// SHA-256 as 64 lowercase hex chars, via Windows CNG (bcrypt) — no OpenSSL.
[[nodiscard]] std::expected<std::string, std::string> sha256File(const std::filesystem::path& file);
[[nodiscard]] std::expected<std::string, std::string> sha256Bytes(std::string_view bytes);

}

#include "manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace updater {
namespace {

using json = nlohmann::json;

bool isLowerHex64(const std::string& text) {
    if (text.size() != 64) {
        return false;
    }
    return std::ranges::all_of(text, [](const char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool isSlug(const std::string& text) {
    if (text.empty() || text.size() > 32) {
        return false;
    }
    const auto isSlugChar = [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
    };
    return isSlugChar(text.front()) && text.front() != '-' && std::ranges::all_of(text, isSlugChar);
}

bool isValidVersion(const std::string& text) {
    std::string_view rest{text};
    for (int part = 0; part < 3; ++part) {
        const auto dot = rest.find('.');
        const auto piece = part == 2 ? rest : rest.substr(0, dot);
        if (piece.empty() || piece.size() > 9 || (piece.size() > 1 && piece.front() == '0')) {
            return false;
        }
        // Digits only — from_chars would happily take a leading '-'.
        if (!std::ranges::all_of(piece, [](const char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        int value = 0;
        const auto [ptr, ec] = std::from_chars(piece.data(), piece.data() + piece.size(), value);
        if (ec != std::errc{} || ptr != piece.data() + piece.size()) {
            return false;
        }
        if (part != 2) {
            if (dot == std::string_view::npos) {
                return false;
            }
            rest = rest.substr(dot + 1);
        }
    }
    return true;
}

bool isSafeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 512) {
        return false;
    }
    if (path.front() == '/' || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos) {
        return false;
    }
    for (const char character : path) {
        const auto code = static_cast<unsigned char>(character);
        if (code < 0x20 || code == 0x7F) {
            return false;
        }
    }
    std::string_view rest{path};
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto segment = rest.substr(0, slash);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    return !(path == ".update" || path.starts_with(".update/"));
}

std::string lowered(std::string text) {
    for (auto& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

}  // namespace

std::expected<Manifest, std::string> Manifest::parse(const std::string& bytes) {
    json document;
    try {
        document = json::parse(bytes);
    } catch (const json::exception& exception) {
        return std::unexpected(std::string("manifest is not valid JSON: ") + exception.what());
    }
    if (!document.is_object()) {
        return std::unexpected("manifest must be a JSON object");
    }

    Manifest manifest;
    try {
        manifest.schema = document.at("schema").get<int>();
        manifest.keyId = document.at("key_id").get<std::string>();
        manifest.version = document.at("version").get<std::string>();
        manifest.platform = document.at("platform").get<std::string>();
        manifest.channel = document.at("channel").get<std::string>();
        manifest.minSupportedVersion = document.at("min_supported_version").get<std::string>();
        manifest.createdAt = document.at("created_at").get<std::string>();
        manifest.notes = document.value("notes", "");
        for (const auto& entry : document.at("files")) {
            manifest.files.push_back(ManifestFile{
                .path = entry.at("path").get<std::string>(),
                .sha256 = entry.at("sha256").get<std::string>(),
                .size = entry.at("size").get<std::int64_t>(),
            });
        }
    } catch (const json::exception& exception) {
        return std::unexpected(std::string("manifest field missing or mistyped: ") + exception.what());
    }

    if (manifest.schema != 1) {
        return std::unexpected("unsupported manifest schema");
    }
    if (manifest.keyId.empty() || manifest.keyId.size() > 64) {
        return std::unexpected("key_id is required");
    }
    if (!isValidVersion(manifest.version)) {
        return std::unexpected("version must be numeric major.minor.patch");
    }
    if (!isValidVersion(manifest.minSupportedVersion)) {
        return std::unexpected("min_supported_version must be numeric major.minor.patch");
    }
    if (!isSlug(manifest.platform)) {
        return std::unexpected("platform must match [a-z0-9][a-z0-9-]*");
    }
    if (!isSlug(manifest.channel)) {
        return std::unexpected("channel must match [a-z0-9][a-z0-9-]*");
    }
    if (manifest.createdAt.empty()) {
        return std::unexpected("created_at is required");
    }
    if (manifest.files.empty()) {
        return std::unexpected("files must not be empty");
    }

    std::unordered_set<std::string> seenPaths;
    for (const auto& file : manifest.files) {
        if (!isSafeRelativePath(file.path)) {
            return std::unexpected("unsafe file path: " + file.path);
        }
        if (!isLowerHex64(file.sha256)) {
            return std::unexpected("sha256 must be 64 lowercase hex chars: " + file.path);
        }
        if (file.size < 0) {
            return std::unexpected("size must be non-negative: " + file.path);
        }
        if (!seenPaths.insert(lowered(file.path)).second) {
            return std::unexpected("duplicate file path: " + file.path);
        }
    }

    if (manifest.platform.starts_with("win")) {
        for (const char* required : {"Client.exe", "updater.exe"}) {
            if (!seenPaths.contains(lowered(required))) {
                return std::unexpected(std::string(required) + " is required in the manifest");
            }
        }
    }
    return manifest;
}

std::vector<FileOp> diffManifests(const Manifest& oldManifest, const Manifest& newManifest) {
    // Keyed case-insensitively: NTFS treats "Foo.dll" and "foo.dll" as one file.
    std::unordered_map<std::string, const ManifestFile*> oldByPath;
    for (const auto& file : oldManifest.files) {
        oldByPath[lowered(file.path)] = &file;
    }

    std::vector<FileOp> ops;
    std::unordered_set<std::string> newPaths;
    for (const auto& file : newManifest.files) {
        const auto key = lowered(file.path);
        newPaths.insert(key);
        const auto existing = oldByPath.find(key);
        if (existing == oldByPath.end()) {
            ops.push_back({FileOp::Kind::Add, file.path, file.sha256});
        } else if (existing->second->sha256 != file.sha256) {
            ops.push_back({FileOp::Kind::Replace, file.path, file.sha256});
        }
    }
    for (const auto& file : oldManifest.files) {
        if (!newPaths.contains(lowered(file.path))) {
            ops.push_back({FileOp::Kind::Remove, file.path, {}});
        }
    }
    return ops;
}

std::vector<FileOp> fullInstallOps(const Manifest& newManifest) {
    std::vector<FileOp> ops;
    ops.reserve(newManifest.files.size());
    for (const auto& file : newManifest.files) {
        ops.push_back({FileOp::Kind::Add, file.path, file.sha256});
    }
    return ops;
}

}

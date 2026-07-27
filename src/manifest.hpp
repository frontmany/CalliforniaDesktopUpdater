#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace updater {

struct ManifestFile {
    std::string path;    // relative, forward slashes
    std::string sha256;  // 64 lowercase hex
    std::int64_t size{0};
};

/// Parsed release manifest. Validation rules mirror the Updates service and
/// the client (three independent implementations at trust boundaries):
///  - schema == 1; version/min_supported_version strict "N.N.N";
///  - platform/channel: [a-z0-9][a-z0-9-]*;
///  - paths: relative, forward slashes, no ".."/"."/empty segments, no drive
///    letters or backslashes, never inside ".update/";
///  - sha256 64 lowercase hex; size >= 0; no case-insensitive duplicate paths;
///  - win-* platforms must ship Client.exe and updater.exe.
struct Manifest {
    int schema{1};
    std::string keyId;
    std::string version;
    std::string platform;
    std::string channel;
    std::string minSupportedVersion;
    std::string createdAt;
    std::string notes;
    std::vector<ManifestFile> files;

    static std::expected<Manifest, std::string> parse(const std::string& bytes);
};

/// One planned filesystem operation of an update.
struct FileOp {
    enum class Kind { Replace, Add, Remove };
    Kind kind{Kind::Add};
    std::string path;
    std::string sha256;  // target hash (empty for Remove)
};

/// Diff old -> new install state. Files present locally but absent from both
/// manifests are never touched.
[[nodiscard]] std::vector<FileOp> diffManifests(const Manifest& oldManifest,
                                                const Manifest& newManifest);

/// Same diff, but for a fresh/unknown install (no local manifest): everything
/// in `newManifest` becomes Add.
[[nodiscard]] std::vector<FileOp> fullInstallOps(const Manifest& newManifest);

}

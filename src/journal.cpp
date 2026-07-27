#include "journal.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace updater {
namespace {

using json = nlohmann::json;

const char* kindToText(const FileOp::Kind kind) {
    switch (kind) {
    case FileOp::Kind::Replace: return "replace";
    case FileOp::Kind::Add: return "add";
    case FileOp::Kind::Remove: return "remove";
    }
    return "add";
}

std::optional<FileOp::Kind> kindFromText(const std::string& text) {
    if (text == "replace") {
        return FileOp::Kind::Replace;
    }
    if (text == "add") {
        return FileOp::Kind::Add;
    }
    if (text == "remove") {
        return FileOp::Kind::Remove;
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path Journal::pathIn(const std::filesystem::path& installDir) {
    return installDir / L".update" / L"journal.json";
}

std::optional<Journal> Journal::load(const std::filesystem::path& installDir) {
    std::ifstream file{pathIn(installDir), std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    json document;
    try {
        document = json::parse(buffer.str());
        Journal journal;
        journal.state = document.at("state").get<std::string>();
        journal.version = document.at("version").get<std::string>();
        journal.relaunchExe = document.at("relaunch_exe").get<std::string>();
        for (const auto& entry : document.at("ops")) {
            const auto kind = kindFromText(entry.at("op").get<std::string>());
            if (!kind) {
                return std::nullopt;
            }
            journal.ops.push_back(FileOp{
                .kind = *kind,
                .path = entry.at("path").get<std::string>(),
                .sha256 = entry.value("sha256", ""),
            });
        }
        if (journal.state != "staged" && journal.state != "swapping") {
            return std::nullopt;
        }
        return journal;
    } catch (const json::exception&) {
        // Corrupt journal: treat as absent; repair will fall back to cleanup.
        return std::nullopt;
    }
}

std::expected<void, std::string> Journal::save(const std::filesystem::path& installDir) const {
    json ops_json = json::array();
    for (const auto& op : ops) {
        ops_json.push_back({{"op", kindToText(op.kind)}, {"path", op.path}, {"sha256", op.sha256}});
    }
    const json document{
        {"state", state},
        {"version", version},
        {"relaunch_exe", relaunchExe},
        {"ops", std::move(ops_json)},
    };

    const auto target = pathIn(installDir);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    const auto temp = target.wstring() + L".tmp";
    {
        std::ofstream file{std::filesystem::path{temp}, std::ios::binary | std::ios::trunc};
        if (!file) {
            return std::unexpected("cannot write journal temp file");
        }
        const auto text = document.dump(2);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file.good()) {
            return std::unexpected("journal write failed");
        }
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        // rename() cannot replace on Windows when the target exists; remove and retry.
        std::filesystem::remove(target, ec);
        std::filesystem::rename(temp, target, ec);
        if (ec) {
            return std::unexpected("journal rename failed: " + ec.message());
        }
    }
    return {};
}

void Journal::remove(const std::filesystem::path& installDir) {
    std::error_code ec;
    std::filesystem::remove(pathIn(installDir), ec);
}

}

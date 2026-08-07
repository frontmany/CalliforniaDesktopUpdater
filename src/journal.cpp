#include "journal.hpp"

#include "fsops.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <utility>

namespace updater {
namespace {

using json = nlohmann::json;

const char* kindToText(const FileOp::Kind kind) {
    switch (kind) {
    case FileOp::Kind::Replace: return "replace";
    case FileOp::Kind::Add: return "add";
    case FileOp::Kind::Remove: return "remove";
    }
    std::unreachable();
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

std::expected<std::optional<Journal>, std::string> Journal::load(
    const std::filesystem::path& installDir) {
    std::ifstream file{pathIn(installDir), std::ios::binary};
    if (!file) {
        return std::optional<Journal>{};  // no journal on disk
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    try {
        const json document = json::parse(buffer.str());
        Journal journal;
        journal.state = document.at("state").get<std::string>();
        journal.version = document.at("version").get<std::string>();
        journal.relaunchExe = document.at("relaunch_exe").get<std::string>();
        for (const auto& entry : document.at("ops")) {
            const auto kind = kindFromText(entry.at("op").get<std::string>());
            if (!kind) {
                return std::unexpected("journal has unknown op kind: " +
                                       entry.at("op").get<std::string>());
            }
            journal.ops.push_back(FileOp{
                .kind = *kind,
                .path = entry.at("path").get<std::string>(),
                .sha256 = entry.value("sha256", ""),
            });
        }
        if (journal.state != "staged" && journal.state != "swapping") {
            return std::unexpected("journal has unknown state: " + journal.state);
        }
        return std::optional<Journal>{std::move(journal)};
    } catch (const json::exception& exception) {
        return std::unexpected(std::string("journal unparseable: ") + exception.what());
    }
}

std::expected<void, std::string> Journal::save(const std::filesystem::path& installDir) const {
    json opsJson = json::array();
    for (const auto& op : ops) {
        opsJson.push_back({{"op", kindToText(op.kind)}, {"path", op.path}, {"sha256", op.sha256}});
    }
    const json document{
        {"state", state},
        {"version", version},
        {"relaunch_exe", relaunchExe},
        {"ops", std::move(opsJson)},
    };

    const auto target = pathIn(installDir);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected("cannot create journal directory: " + ec.message());
    }
    const auto temp = std::filesystem::path{target.wstring() + L".tmp"};
    {
        std::ofstream file{temp, std::ios::binary | std::ios::trunc};
        if (!file) {
            return std::unexpected("cannot write journal temp file");
        }
        const auto text = document.dump(2);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        if (!file.good()) {
            return std::unexpected("journal write failed");
        }
    }
    // Atomic replace: at no point is there a populated .update tree without a
    // journal (a crash there would make repair mistake it for leftovers).
    if (const auto replaced = fsops::retryReplaceFile(temp, target); !replaced) {
        return std::unexpected("journal rename failed: " + replaced.error());
    }
    return {};
}

bool Journal::remove(const std::filesystem::path& installDir) {
    std::error_code ec;
    std::filesystem::remove(pathIn(installDir), ec);
    return !std::filesystem::exists(pathIn(installDir), ec);
}

}  // namespace updater

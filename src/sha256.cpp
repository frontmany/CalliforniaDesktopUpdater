#include "sha256.hpp"

#include "win_raii.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdio>
#include <vector>

namespace updater {
namespace {

constexpr std::size_t kDigestLength = 32;

std::string toHex(const unsigned char* data, const std::size_t length) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

/// RAII-ish streaming hasher over CNG.
class Hasher {
public:
    Hasher() {
        if (!BCRYPT_SUCCESS(
                BCryptOpenAlgorithmProvider(&mAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
            return;
        }
        if (!BCRYPT_SUCCESS(BCryptCreateHash(mAlgorithm, &mHash, nullptr, 0, nullptr, 0, 0))) {
            mHash = nullptr;
        }
    }

    // Copying would double-destroy the CNG handles.
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    ~Hasher() {
        if (mHash != nullptr) {
            BCryptDestroyHash(mHash);
        }
        if (mAlgorithm != nullptr) {
            BCryptCloseAlgorithmProvider(mAlgorithm, 0);
        }
    }

    [[nodiscard]] bool valid() const { return mHash != nullptr; }

    [[nodiscard]] bool update(const void* data, const std::size_t length) {
        return BCRYPT_SUCCESS(BCryptHashData(
            mHash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(length), 0));
    }

    [[nodiscard]] std::expected<std::string, std::string> finish() {
        std::array<unsigned char, kDigestLength> digest{};
        if (!BCRYPT_SUCCESS(
                BCryptFinishHash(mHash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
            return std::unexpected("BCryptFinishHash failed");
        }
        return toHex(digest.data(), digest.size());
    }

private:
    BCRYPT_ALG_HANDLE mAlgorithm{nullptr};
    BCRYPT_HASH_HANDLE mHash{nullptr};
};

}  // namespace

std::expected<std::string, std::string> sha256File(const std::filesystem::path& file) {
    Hasher hasher;
    if (!hasher.valid()) {
        return std::unexpected("CNG SHA-256 unavailable");
    }
    const UniqueFile handle{_wfopen(file.c_str(), L"rb")};
    if (!handle.valid()) {
        return std::unexpected("cannot open file: " + file.string());
    }
    std::vector<unsigned char> buffer(1 << 20);
    for (;;) {
        const auto read = std::fread(buffer.data(), 1, buffer.size(), handle.get());
        if (read > 0 && !hasher.update(buffer.data(), read)) {
            return std::unexpected("BCryptHashData failed");
        }
        if (read < buffer.size()) {
            if (std::ferror(handle.get()) != 0) {
                return std::unexpected("read error: " + file.string());
            }
            break;
        }
    }
    return hasher.finish();
}

std::expected<std::string, std::string> sha256Bytes(const std::string_view bytes) {
    Hasher hasher;
    if (!hasher.valid()) {
        return std::unexpected("CNG SHA-256 unavailable");
    }
    if (!bytes.empty() && !hasher.update(bytes.data(), bytes.size())) {
        return std::unexpected("BCryptHashData failed");
    }
    return hasher.finish();
}

}  // namespace updater

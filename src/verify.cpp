#include "verify.hpp"

#include "public_key.hpp"

#include <ed25519.h>

#include <array>
#include <cstdint>

namespace updater {
namespace {

constexpr std::array<std::int8_t, 256> buildReverse() {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<std::int8_t, 256> table{};
    for (auto& value : table) {
        value = -1;
    }
    for (int i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kAlphabet[static_cast<std::size_t>(i)])] =
            static_cast<std::int8_t>(i);
    }
    return table;
}

constexpr auto kReverse = buildReverse();

}  // namespace

bool verifyManifestSignature(const std::string_view message, const std::string_view signature) {
    if (signature.size() != 64) {
        return false;
    }
    return ed25519_verify(reinterpret_cast<const unsigned char*>(signature.data()),
                          reinterpret_cast<const unsigned char*>(message.data()),
                          message.size(),
                          kUpdatePublicKey.data()) == 1;
}

bool base64Decode(const std::string_view text, std::string& out) {
    out.clear();
    if (text.size() % 4 != 0) {
        return false;
    }
    out.reserve(text.size() / 4 * 3);
    for (std::size_t index = 0; index < text.size(); index += 4) {
        int values[4];
        int padding = 0;
        for (int position = 0; position < 4; ++position) {
            const char character = text[index + position];
            if (character == '=') {
                if (index + 4 != text.size() || position < 2) {
                    return false;
                }
                values[position] = 0;
                ++padding;
            } else {
                if (padding > 0) {
                    return false;
                }
                const auto value = kReverse[static_cast<unsigned char>(character)];
                if (value < 0) {
                    return false;
                }
                values[position] = value;
            }
        }
        out.push_back(static_cast<char>((values[0] << 2) | (values[1] >> 4)));
        if (padding < 2) {
            out.push_back(static_cast<char>(((values[1] & 0x0F) << 4) | (values[2] >> 2)));
        }
        if (padding < 1) {
            out.push_back(static_cast<char>(((values[2] & 0x03) << 6) | values[3]));
        }
    }
    return true;
}

}

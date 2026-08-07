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
    if (signature.size() != 64 || message.empty()) {
        // An empty message can never carry a valid manifest; rejecting it here
        // also keeps a potentially-null data() out of the C verify routine.
        return false;
    }
    return ed25519_verify(reinterpret_cast<const unsigned char*>(signature.data()),
                          reinterpret_cast<const unsigned char*>(message.data()),
                          message.size(),
                          kUpdatePublicKey.data()) == 1;
}

std::expected<std::string, std::string> base64Decode(const std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::unexpected("base64 length is not a multiple of 4");
    }
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t index = 0; index < text.size(); index += 4) {
        int values[4];
        int padding = 0;
        for (int position = 0; position < 4; ++position) {
            const char character = text[index + position];
            if (character == '=') {
                if (index + 4 != text.size() || position < 2) {
                    return std::unexpected("base64 padding in an illegal position");
                }
                values[position] = 0;
                ++padding;
            } else {
                if (padding > 0) {
                    return std::unexpected("base64 data after padding");
                }
                const auto value = kReverse[static_cast<unsigned char>(character)];
                if (value < 0) {
                    return std::unexpected("base64 contains an invalid character");
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
    return out;
}

}  // namespace updater

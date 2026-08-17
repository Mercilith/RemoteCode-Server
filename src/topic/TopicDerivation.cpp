#include "TopicDerivation.h"

#include <array>
#include <cstring>

#include "../third_party/monocypher.h"

namespace {

constexpr int kTopicHashBytes = 16; // 32 hex chars — plenty of entropy, keeps the topic short

std::array<uint8_t, 8> EncodeEpochBigEndian(int64_t epoch) {
    std::array<uint8_t, 8> out{};
    uint64_t value = static_cast<uint64_t>(epoch);
    for (int i = 7; i >= 0; --i) {
        out[static_cast<size_t>(i)] = static_cast<uint8_t>(value & 0xFF);
        value >>= 8;
    }
    return out;
}

std::string HexEncode(const uint8_t* bytes, size_t len) {
    static const char* kHexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHexDigits[bytes[i] >> 4]);
        out.push_back(kHexDigits[bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace

int64_t TopicDerivation::CurrentEpoch(int64_t unixTimeSeconds) {
    return unixTimeSeconds / kEpochSeconds;
}

int64_t TopicDerivation::SecondsUntilNextEpoch(int64_t unixTimeSeconds) {
    const int64_t intoEpoch = unixTimeSeconds % kEpochSeconds;
    return kEpochSeconds - intoEpoch;
}

std::string TopicDerivation::DeriveTopicForEpoch(const uint8_t* publicKey, int64_t epoch) {
    const std::array<uint8_t, 8> epochBytes = EncodeEpochBigEndian(epoch);

    uint8_t message[32 + 8];
    memcpy(message, publicKey, 32);
    memcpy(message + 32, epochBytes.data(), 8);

    uint8_t hash[kTopicHashBytes];
    crypto_blake2b(hash, kTopicHashBytes, message, sizeof(message));

    return HexEncode(hash, kTopicHashBytes);
}

std::string TopicDerivation::DeriveTopic(const uint8_t* publicKey, int64_t unixTimeSeconds) {
    return DeriveTopicForEpoch(publicKey, CurrentEpoch(unixTimeSeconds));
}

#pragma once

#include <cstdint>
#include <string>

// Derives the rotating MQTT topic the server listens on. The topic is
// `hex(BLAKE2b(publicKey || epoch))`, where `epoch = unixTimeSeconds / 300`
// (integer division) — a new topic every 5 minutes. No separate "since UTC
// midnight" math is needed: the Unix epoch (1970-01-01T00:00:00 UTC) is
// itself a UTC-midnight instant, so plain integer division already
// produces boundaries aligned to UTC midnight in 5-minute steps.
//
// The topic isn't a secret — MQTT topics are visible to the broker and to
// anyone who guesses them. Folding the public key into the hash means only
// someone who has seen it (i.e., paired with this server) can predict the
// next topic; the 5-minute rotation limits how long any given topic stays
// meaningful to guess even if it leaks.
class TopicDerivation {
public:
    static constexpr int64_t kEpochSeconds = 300;

    static int64_t CurrentEpoch(int64_t unixTimeSeconds);
    static int64_t SecondsUntilNextEpoch(int64_t unixTimeSeconds);

    // `publicKey` must point to 32 bytes.
    static std::string DeriveTopicForEpoch(const uint8_t* publicKey, int64_t epoch);
    static std::string DeriveTopic(const uint8_t* publicKey, int64_t unixTimeSeconds);
};

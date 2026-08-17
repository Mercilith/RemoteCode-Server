#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Pure MQTT 3.1.1 packet encoding/decoding — no network or Windows
// dependencies, so it's directly unit-testable. Deliberately minimal: QoS 0
// only (no packet-id bookkeeping needed for PUBLISH), clean sessions, no
// will/username/password. One MQTT control packet per WebSocket binary
// message, which is how MQTT-over-WebSocket is used in practice (and what
// HiveMQ's broker does) — so each decode call assumes its input buffer
// holds exactly one complete packet.
namespace MqttPacket {

enum class Type : uint8_t {
    Connect = 1,
    ConnAck = 2,
    Publish = 3,
    Subscribe = 8,
    SubAck = 9,
    Unsubscribe = 10,
    UnsubAck = 11,
    PingReq = 12,
    PingResp = 13,
    Disconnect = 14,
};

struct FixedHeader {
    Type type{};
    uint8_t flags = 0;
    // Points into the original buffer: [variable header + payload], with
    // the fixed header (type/flags byte + remaining-length bytes) already
    // stripped off.
    std::vector<uint8_t> body;
};

struct ConnAckResult {
    bool success = false;
    uint8_t sessionPresent = 0;
    uint8_t returnCode = 0;
};

struct SubAckResult {
    bool success = false;
    uint16_t packetId = 0;
    uint8_t returnCode = 0;
};

struct PublishResult {
    bool success = false;
    std::string topic;
    std::vector<uint8_t> payload;
};

// Parses the fixed header (packet type, flags, remaining length) from a
// buffer assumed to hold exactly one complete packet. Returns false if the
// buffer is malformed or the declared remaining length doesn't match what
// followed.
bool DecodeFixedHeader(const std::vector<uint8_t>& data, FixedHeader& out);

std::vector<uint8_t> EncodeConnect(const std::string& clientId, uint16_t keepAliveSeconds);
ConnAckResult DecodeConnAck(const FixedHeader& header);

std::vector<uint8_t> EncodeSubscribe(uint16_t packetId, const std::string& topic);
SubAckResult DecodeSubAck(const FixedHeader& header);

std::vector<uint8_t> EncodeUnsubscribe(uint16_t packetId, const std::string& topic);

std::vector<uint8_t> EncodePublish(
    const std::string& topic, const std::vector<uint8_t>& payload, bool retain = false);
PublishResult DecodePublish(const FixedHeader& header);

std::vector<uint8_t> EncodePingReq();
bool IsPingResp(const FixedHeader& header);

std::vector<uint8_t> EncodeDisconnect();

} // namespace MqttPacket

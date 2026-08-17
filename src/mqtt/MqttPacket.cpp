#include "MqttPacket.h"

namespace MqttPacket {

namespace {

void EncodeRemainingLength(uint32_t length, std::vector<uint8_t>& out) {
    do {
        uint8_t byte = static_cast<uint8_t>(length % 128);
        length /= 128;
        if (length > 0) {
            byte |= 0x80;
        }
        out.push_back(byte);
    } while (length > 0);
}

// Decodes the variable-length "remaining length" field starting at
// data[offset], advancing offset past it. Returns false if malformed
// (more than 4 continuation bytes) or the buffer runs out.
bool DecodeRemainingLength(const std::vector<uint8_t>& data, size_t& offset, uint32_t& outLength) {
    uint32_t multiplier = 1;
    uint32_t value = 0;
    uint8_t byte;
    int bytesRead = 0;
    do {
        if (offset >= data.size() || bytesRead >= 4) {
            return false;
        }
        byte = data[offset++];
        value += static_cast<uint32_t>(byte & 0x7F) * multiplier;
        multiplier *= 128;
        ++bytesRead;
    } while ((byte & 0x80) != 0);
    outLength = value;
    return true;
}

void EncodeString(const std::string& s, std::vector<uint8_t>& out) {
    const uint16_t len = static_cast<uint16_t>(s.size());
    out.push_back(static_cast<uint8_t>(len >> 8));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.insert(out.end(), s.begin(), s.end());
}

// Reads a length-prefixed UTF-8 string from body[offset..], advancing
// offset. Returns false if the buffer is too short for the declared length.
bool DecodeString(const std::vector<uint8_t>& body, size_t& offset, std::string& out) {
    if (offset + 2 > body.size()) {
        return false;
    }
    const uint16_t len = (static_cast<uint16_t>(body[offset]) << 8) | body[offset + 1];
    offset += 2;
    if (offset + len > body.size()) {
        return false;
    }
    out.assign(body.begin() + static_cast<long>(offset), body.begin() + static_cast<long>(offset + len));
    offset += len;
    return true;
}

std::vector<uint8_t> AssemblePacket(Type type, uint8_t flags, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | (flags & 0x0F)));
    EncodeRemainingLength(static_cast<uint32_t>(body.size()), out);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

} // namespace

bool DecodeFixedHeader(const std::vector<uint8_t>& data, FixedHeader& out) {
    if (data.empty()) {
        return false;
    }
    const uint8_t first = data[0];
    out.type = static_cast<Type>(first >> 4);
    out.flags = first & 0x0F;

    size_t offset = 1;
    uint32_t remainingLength = 0;
    if (!DecodeRemainingLength(data, offset, remainingLength)) {
        return false;
    }
    if (offset + remainingLength != data.size()) {
        return false; // declared length doesn't match what actually followed
    }
    out.body.assign(
        data.begin() + static_cast<long>(offset), data.begin() + static_cast<long>(offset + remainingLength));
    return true;
}

std::vector<uint8_t> EncodeConnect(const std::string& clientId, uint16_t keepAliveSeconds) {
    std::vector<uint8_t> body;
    EncodeString("MQTT", body); // protocol name
    body.push_back(4);          // protocol level 4 = MQTT 3.1.1
    body.push_back(0x02);       // connect flags: clean session, no will/user/pass
    body.push_back(static_cast<uint8_t>(keepAliveSeconds >> 8));
    body.push_back(static_cast<uint8_t>(keepAliveSeconds & 0xFF));
    EncodeString(clientId, body);
    return AssemblePacket(Type::Connect, 0, body);
}

ConnAckResult DecodeConnAck(const FixedHeader& header) {
    ConnAckResult result;
    if (header.type != Type::ConnAck || header.body.size() != 2) {
        return result;
    }
    result.sessionPresent = header.body[0] & 0x01;
    result.returnCode = header.body[1];
    result.success = result.returnCode == 0;
    return result;
}

std::vector<uint8_t> EncodeSubscribe(uint16_t packetId, const std::string& topic) {
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(packetId >> 8));
    body.push_back(static_cast<uint8_t>(packetId & 0xFF));
    EncodeString(topic, body);
    body.push_back(0); // requested QoS 0
    // SUBSCRIBE's fixed header flags must be 0b0010 per the spec.
    return AssemblePacket(Type::Subscribe, 0x02, body);
}

SubAckResult DecodeSubAck(const FixedHeader& header) {
    SubAckResult result;
    if (header.type != Type::SubAck || header.body.size() < 3) {
        return result;
    }
    result.packetId = (static_cast<uint16_t>(header.body[0]) << 8) | header.body[1];
    result.returnCode = header.body[2];
    result.success = result.returnCode != 0x80; // 0x80 = failure
    return result;
}

std::vector<uint8_t> EncodeUnsubscribe(uint16_t packetId, const std::string& topic) {
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(packetId >> 8));
    body.push_back(static_cast<uint8_t>(packetId & 0xFF));
    EncodeString(topic, body);
    return AssemblePacket(Type::Unsubscribe, 0x02, body);
}

std::vector<uint8_t> EncodePublish(
    const std::string& topic, const std::vector<uint8_t>& payload, bool retain) {
    std::vector<uint8_t> body;
    EncodeString(topic, body);
    // QoS 0: no packet identifier field.
    body.insert(body.end(), payload.begin(), payload.end());
    const uint8_t flags = retain ? 0x01 : 0x00;
    return AssemblePacket(Type::Publish, flags, body);
}

PublishResult DecodePublish(const FixedHeader& header) {
    PublishResult result;
    if (header.type != Type::Publish) {
        return result;
    }
    const uint8_t qos = (header.flags >> 1) & 0x03;

    size_t offset = 0;
    std::string topic;
    if (!DecodeString(header.body, offset, topic)) {
        return result;
    }
    if (qos > 0) {
        offset += 2; // skip packet identifier — this client only ever subscribes at QoS 0
    }
    if (offset > header.body.size()) {
        return result;
    }

    result.topic = std::move(topic);
    result.payload.assign(header.body.begin() + static_cast<long>(offset), header.body.end());
    result.success = true;
    return result;
}

std::vector<uint8_t> EncodePingReq() {
    return AssemblePacket(Type::PingReq, 0, {});
}

bool IsPingResp(const FixedHeader& header) {
    return header.type == Type::PingResp;
}

std::vector<uint8_t> EncodeDisconnect() {
    return AssemblePacket(Type::Disconnect, 0, {});
}

} // namespace MqttPacket

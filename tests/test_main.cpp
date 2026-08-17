#include <iostream>
#include <string>

#include "../src/greeting.h"
#include "mqtt/MqttPacket.h"
#include "topic/TopicDerivation.h"

namespace {

int failures = 0;

void Check(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << std::endl;
        ++failures;
    }
}

void TestGreeting() {
    Check(greeting() == "Hello, World!", "greeting() returns the expected string");
}

void TestTopicDerivation() {
    uint8_t pubkey[32];
    for (int i = 0; i < 32; ++i) {
        pubkey[i] = static_cast<uint8_t>(i);
    }

    // Reference values computed independently via Python's hashlib.blake2b
    // (BLAKE2b(pubkey || epoch_as_8_byte_big_endian), digest_size=16).
    Check(
        TopicDerivation::DeriveTopicForEpoch(pubkey, 12345) == "2102dff634621c72e3929c96e12cee6e",
        "Topic for epoch 12345 matches the independently-computed BLAKE2b reference value");
    Check(
        TopicDerivation::DeriveTopicForEpoch(pubkey, 0) == "1b6a0a281f1a641a32ff0953278d6b8c",
        "Topic for epoch 0 matches the independently-computed BLAKE2b reference value");
    Check(
        TopicDerivation::DeriveTopicForEpoch(pubkey, 5000000000LL) ==
            "1a5c619f3d8fb49b099b554b62b0dcae",
        "Topic for a large epoch matches the independently-computed BLAKE2b reference value");

    Check(TopicDerivation::CurrentEpoch(0) == 0, "Epoch 0 is unix time 0 (a UTC-midnight instant)");
    Check(TopicDerivation::CurrentEpoch(299) == 0, "Epoch 0 covers unix time up to 299 inclusive");
    Check(TopicDerivation::CurrentEpoch(300) == 1, "Epoch 1 begins exactly at unix time 300");
    Check(
        TopicDerivation::CurrentEpoch(86400) == 288,
        "24 hours in is epoch 288 (86400/300) — still aligned to UTC midnight");

    Check(
        TopicDerivation::SecondsUntilNextEpoch(0) == 300,
        "300 seconds remain right at an epoch boundary");
    Check(
        TopicDerivation::SecondsUntilNextEpoch(299) == 1,
        "1 second remains just before the next epoch boundary");

    uint8_t otherPubkey[32] = {};
    Check(
        TopicDerivation::DeriveTopicForEpoch(pubkey, 100) !=
            TopicDerivation::DeriveTopicForEpoch(otherPubkey, 100),
        "Different public keys produce different topics for the same epoch");
    Check(
        TopicDerivation::DeriveTopicForEpoch(pubkey, 100) !=
            TopicDerivation::DeriveTopicForEpoch(pubkey, 101),
        "Different epochs produce different topics for the same public key");
    Check(
        TopicDerivation::DeriveTopicForEpoch(pubkey, 100).size() == 32,
        "Topic is 32 hex characters (16 bytes)");
}

void TestMqttPacketConnect() {
    const std::vector<uint8_t> packet = MqttPacket::EncodeConnect("client1", 60);

    MqttPacket::FixedHeader header;
    Check(MqttPacket::DecodeFixedHeader(packet, header), "CONNECT packet round-trips through DecodeFixedHeader");
    Check(header.type == MqttPacket::Type::Connect, "Decoded fixed header reports type Connect");

    // Fixed header byte: (CONNECT=1)<<4 | flags(0) = 0x10.
    Check(packet[0] == 0x10, "CONNECT fixed header byte is 0x10");
}

void TestMqttPacketConnAck() {
    // type=CONNACK(2), flags=0 -> 0x20; remaining length 2; session-present=0; return-code=0 (accepted).
    const std::vector<uint8_t> raw = {0x20, 0x02, 0x00, 0x00};
    MqttPacket::FixedHeader header;
    Check(MqttPacket::DecodeFixedHeader(raw, header), "Hand-built CONNACK bytes decode via DecodeFixedHeader");
    const MqttPacket::ConnAckResult result = MqttPacket::DecodeConnAck(header);
    Check(result.success, "CONNACK with return code 0 is reported as success");

    const std::vector<uint8_t> rejected = {0x20, 0x02, 0x00, 0x05};
    MqttPacket::FixedHeader rejectedHeader;
    MqttPacket::DecodeFixedHeader(rejected, rejectedHeader);
    Check(!MqttPacket::DecodeConnAck(rejectedHeader).success, "CONNACK with a non-zero return code is reported as failure");
}

void TestMqttPacketSubscribeSubAck() {
    const std::vector<uint8_t> subscribe = MqttPacket::EncodeSubscribe(42, "some/topic");
    Check((subscribe[0] & 0x0F) == 0x02, "SUBSCRIBE fixed header flags are 0b0010 per the MQTT spec");

    MqttPacket::FixedHeader header;
    MqttPacket::DecodeFixedHeader(subscribe, header);
    Check(header.type == MqttPacket::Type::Subscribe, "Encoded SUBSCRIBE decodes back to type Subscribe");

    // SUBACK: type=9, flags=0 -> 0x90; remaining length 3; packet id 42 (0x002A); return code 0.
    const std::vector<uint8_t> suback = {0x90, 0x03, 0x00, 0x2A, 0x00};
    MqttPacket::FixedHeader subackHeader;
    MqttPacket::DecodeFixedHeader(suback, subackHeader);
    const MqttPacket::SubAckResult result = MqttPacket::DecodeSubAck(subackHeader);
    Check(result.success, "SUBACK with return code 0 is reported as success");
    Check(result.packetId == 42, "SUBACK packet id round-trips correctly");
}

void TestMqttPacketPublish() {
    const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<uint8_t> packet = MqttPacket::EncodePublish("a/b/c", payload);

    MqttPacket::FixedHeader header;
    Check(MqttPacket::DecodeFixedHeader(packet, header), "PUBLISH packet round-trips through DecodeFixedHeader");
    const MqttPacket::PublishResult result = MqttPacket::DecodePublish(header);
    Check(result.success, "PUBLISH decodes successfully");
    Check(result.topic == "a/b/c", "PUBLISH topic round-trips correctly");
    Check(result.payload == payload, "PUBLISH payload bytes round-trip exactly");
}

void TestMqttPacketRemainingLengthMultiByte() {
    // A payload just over 127 bytes forces the remaining-length field to
    // span two bytes (the MQTT variable-length-int continuation case).
    const std::vector<uint8_t> payload(200, 0xAB);
    const std::vector<uint8_t> packet = MqttPacket::EncodePublish("t", payload);

    MqttPacket::FixedHeader header;
    Check(
        MqttPacket::DecodeFixedHeader(packet, header),
        "A packet with a multi-byte remaining-length field decodes correctly");
    const MqttPacket::PublishResult result = MqttPacket::DecodePublish(header);
    Check(result.payload.size() == 200, "Multi-byte remaining-length payload size round-trips correctly");
}

void TestMqttPacketPingAndDisconnect() {
    const std::vector<uint8_t> ping = MqttPacket::EncodePingReq();
    Check(ping == std::vector<uint8_t>({0xC0, 0x00}), "PINGREQ is exactly the two well-known bytes");

    MqttPacket::FixedHeader pingRespHeader;
    MqttPacket::DecodeFixedHeader({0xD0, 0x00}, pingRespHeader);
    Check(MqttPacket::IsPingResp(pingRespHeader), "0xD0 0x00 is recognized as PINGRESP");

    const std::vector<uint8_t> disconnect = MqttPacket::EncodeDisconnect();
    Check(disconnect == std::vector<uint8_t>({0xE0, 0x00}), "DISCONNECT is exactly the two well-known bytes");
}

void TestMqttPacketMalformed() {
    MqttPacket::FixedHeader header;
    Check(!MqttPacket::DecodeFixedHeader({}, header), "An empty buffer is rejected");
    Check(
        !MqttPacket::DecodeFixedHeader({0x30, 0x05, 0x01}, header),
        "A declared remaining-length longer than the actual buffer is rejected");
}

} // namespace

int main() {
    TestGreeting();
    TestTopicDerivation();
    TestMqttPacketConnect();
    TestMqttPacketConnAck();
    TestMqttPacketSubscribeSubAck();
    TestMqttPacketPublish();
    TestMqttPacketRemainingLengthMultiByte();
    TestMqttPacketPingAndDisconnect();
    TestMqttPacketMalformed();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed." << std::endl;
        return 1;
    }
    std::cout << "All tests passed." << std::endl;
    return 0;
}

#include "MqttClient.h"

#include <chrono>

#include "MqttPacket.h"

namespace {
// Bounds how long each receive blocks with no data, so the service's main
// loop wakes often enough to notice a stop request promptly — the
// installer's own service-stop wait (see RemoteCode-Installer's
// PathAndStartupInstaller::WaitForServiceState) only waits 10s before
// giving up, so this needs enough headroom under that for a clean stop.
constexpr DWORD kReceiveTimeoutMs = 5000;
}

bool MqttClient::Connect(
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& webSocketPath,
    const std::string& clientId) {
    if (!transport_.Connect(host, port, webSocketPath, L"mqtt")) {
        return false;
    }
    transport_.SetReceiveTimeoutMs(kReceiveTimeoutMs);

    const std::vector<uint8_t> connectPacket = MqttPacket::EncodeConnect(clientId, keepAliveSeconds_);
    if (!transport_.SendBinary(connectPacket.data(), connectPacket.size())) {
        return false;
    }

    std::vector<uint8_t> response;
    if (!transport_.ReceiveBinary(response)) {
        return false;
    }
    MqttPacket::FixedHeader header;
    if (!MqttPacket::DecodeFixedHeader(response, header)) {
        return false;
    }
    return MqttPacket::DecodeConnAck(header).success;
}

bool MqttClient::Subscribe(const std::string& topic) {
    const uint16_t packetId = nextPacketId_++;
    const std::vector<uint8_t> packet = MqttPacket::EncodeSubscribe(packetId, topic);
    if (!transport_.SendBinary(packet.data(), packet.size())) {
        return false;
    }

    std::vector<uint8_t> response;
    if (!transport_.ReceiveBinary(response)) {
        return false;
    }
    MqttPacket::FixedHeader header;
    if (!MqttPacket::DecodeFixedHeader(response, header)) {
        return false;
    }
    const MqttPacket::SubAckResult result = MqttPacket::DecodeSubAck(header);
    return result.success && result.packetId == packetId;
}

bool MqttClient::Unsubscribe(const std::string& topic) {
    const uint16_t packetId = nextPacketId_++;
    const std::vector<uint8_t> packet = MqttPacket::EncodeUnsubscribe(packetId, topic);
    if (!transport_.SendBinary(packet.data(), packet.size())) {
        return false;
    }
    // UNSUBACK carries no useful status beyond "acknowledged" — read it off
    // the wire so it doesn't get misinterpreted as the next PUBLISH by the
    // receive loop, but don't fail the call over it.
    std::vector<uint8_t> response;
    transport_.ReceiveBinary(response);
    return true;
}

bool MqttClient::PumpOnce(const MessageCallback& onMessage) {
    using clock = std::chrono::steady_clock;
    const auto pingInterval = std::chrono::seconds(keepAliveSeconds_ / 2);

    if (clock::now() - lastSend_ >= pingInterval) {
        const std::vector<uint8_t> ping = MqttPacket::EncodePingReq();
        if (!transport_.SendBinary(ping.data(), ping.size())) {
            return false;
        }
        lastSend_ = clock::now();
    }

    std::vector<uint8_t> data;
    if (!transport_.ReceiveBinary(data)) {
        // Either a receive timeout (connection still fine — caller keeps
        // pumping) or a real disconnect, distinguished via IsConnected().
        return transport_.IsConnected();
    }

    MqttPacket::FixedHeader header;
    if (!MqttPacket::DecodeFixedHeader(data, header)) {
        return true;
    }
    if (header.type == MqttPacket::Type::Publish) {
        const MqttPacket::PublishResult publish = MqttPacket::DecodePublish(header);
        if (publish.success && onMessage) {
            onMessage(publish.topic, publish.payload);
        }
    }
    // PingResp and anything else: no action needed beyond having drained
    // it off the wire.
    return true;
}

void MqttClient::RunReceiveLoop(const MessageCallback& onMessage) {
    while (!stopRequested_.load() && transport_.IsConnected()) {
        if (!PumpOnce(onMessage)) {
            break;
        }
    }
}

void MqttClient::Stop() {
    stopRequested_.store(true);
}

void MqttClient::Disconnect() {
    if (transport_.IsConnected()) {
        const std::vector<uint8_t> packet = MqttPacket::EncodeDisconnect();
        transport_.SendBinary(packet.data(), packet.size());
    }
    transport_.Close();
}

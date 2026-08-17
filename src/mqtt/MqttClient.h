#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "WebSocketTransport.h"

// Stateful MQTT-over-WebSocket client tying WebSocketTransport (byte
// transport) to MqttPacket (packet framing). QoS 0 only, one topic
// subscribed at a time (the caller — the service's main loop — handles
// unsubscribe-from-old/subscribe-to-new when the topic rotates).
class MqttClient {
public:
    using MessageCallback =
        std::function<void(const std::string& topic, const std::vector<uint8_t>& payload)>;

    // Connects over wss:// and performs the MQTT CONNECT/CONNACK handshake.
    bool Connect(
        const std::wstring& host,
        INTERNET_PORT port,
        const std::wstring& webSocketPath,
        const std::string& clientId);

    bool Subscribe(const std::string& topic);
    bool Unsubscribe(const std::string& topic);

    // Does one bounded-timeout receive/process cycle (see
    // WebSocketTransport::SetReceiveTimeoutMs) and sends a PINGREQ if more
    // than keepAliveSeconds/2 has elapsed since the last send. Invokes
    // onMessage if a PUBLISH was received this cycle. Returns false once
    // the connection has genuinely dropped (a receive timeout with no data
    // is not a drop — it returns true so the caller keeps pumping). The
    // service's main loop calls this in a tight loop so it can interleave
    // its own per-iteration work (topic-rotation checks, shutdown-event
    // polling) between cycles.
    bool PumpOnce(const MessageCallback& onMessage);

    // Convenience wrapper: calls PumpOnce in a tight loop until Stop() is
    // called from another thread or the connection drops.
    void RunReceiveLoop(const MessageCallback& onMessage);

    // Thread-safe; unblocks RunReceiveLoop/PumpOnce's caller loop the next
    // time the bounded receive wakes.
    void Stop();

    bool IsConnected() const { return transport_.IsConnected(); }

    void Disconnect();

private:
    WebSocketTransport transport_;
    uint16_t nextPacketId_ = 1;
    uint16_t keepAliveSeconds_ = 60;
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point lastSend_ = std::chrono::steady_clock::now();
};

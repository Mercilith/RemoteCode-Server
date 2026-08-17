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

    // Does one receive/process cycle and sends a PINGREQ if more than
    // keepAliveSeconds/2 has elapsed since the last send. Invokes
    // onMessage if a PUBLISH was received this cycle. Returns false once
    // the connection has genuinely dropped. The receive itself blocks
    // until data arrives, the connection drops, or ForceUnblock() is
    // called from another thread — there is no effective per-call
    // timeout (WinHTTP's receive-timeout option does not bound
    // WinHttpWebSocketReceive in practice, confirmed by measurement), so
    // a caller that needs to interleave other per-iteration work (like
    // ServiceMain's topic-rotation check) between cycles cannot rely on
    // PumpOnce returning promptly on its own — see ForceUnblock().
    bool PumpOnce(const MessageCallback& onMessage);

    // Convenience wrapper: calls PumpOnce in a tight loop until Stop() is
    // called from another thread or the connection drops.
    void RunReceiveLoop(const MessageCallback& onMessage);

    // Thread-safe; sets a flag RunReceiveLoop checks between PumpOnce
    // cycles. On its own this does NOT unblock an in-flight PumpOnce —
    // WinHttpWebSocketReceive's timeout option turned out not to actually
    // bound the call in practice (measured a real ~48s service stop before
    // ForceUnblock() was added below), so callers that need a prompt stop
    // while a receive may be in flight should call ForceUnblock() too.
    void Stop();

    // Thread-safe; safe to call from a different thread than the one
    // running PumpOnce/RunReceiveLoop (this is the whole point — it's how
    // ServiceMain's SERVICE_CONTROL_STOP handler, running on the SCM's own
    // control-dispatch thread, interrupts a blocked receive on the main
    // loop's thread). Closes the underlying WebSocket handle, which causes
    // a concurrently-blocked WinHttpWebSocketReceive call to return an
    // error immediately — the standard WinHTTP pattern for cancelling an
    // in-flight synchronous call from another thread.
    void ForceUnblock();

    bool IsConnected() const { return transport_.IsConnected(); }

    void Disconnect();

private:
    WebSocketTransport transport_;
    uint16_t nextPacketId_ = 1;
    uint16_t keepAliveSeconds_ = 60;
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point lastSend_ = std::chrono::steady_clock::now();
};

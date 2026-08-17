#pragma once

#include <windows.h>

#include <winhttp.h>

#include <cstdint>
#include <string>
#include <vector>

// Thin wrapper over WinHTTP's native WebSocket API — reused for its TLS
// handling the same way the RemoteCode-Installer's net/HttpClient.h does
// for plain HTTPS. Gives byte-stream send/receive primitives; MQTT packet
// framing lives one layer up in MqttClient.
class WebSocketTransport {
public:
    ~WebSocketTransport();

    // Connects to wss://host:port/path and requests the given WebSocket
    // subprotocol (MQTT-over-WebSocket requires "mqtt", per the MQTT spec).
    bool Connect(
        const std::wstring& host,
        INTERNET_PORT port,
        const std::wstring& path,
        const std::wstring& subprotocol);

    // Bounds how long ReceiveBinary() blocks with no data — lets the
    // caller's loop wake periodically to check a stop flag, send keep-alive
    // pings, and check for topic rotation, instead of blocking forever.
    // A timed-out receive is reported as a normal false return from
    // ReceiveBinary(), not a fatal error; callers distinguish via
    // IsConnected() if they need to tell "timed out" from "peer closed".
    void SetReceiveTimeoutMs(DWORD timeoutMs);

    bool SendBinary(const uint8_t* data, size_t len);

    // Blocks (up to the receive timeout, if set) until one complete binary
    // message has been received
    // (transparently reassembling multi-fragment messages). Returns false
    // on error or if the peer closed the connection.
    bool ReceiveBinary(std::vector<uint8_t>& outData);

    void Close();

    // False after Close(), or after ReceiveBinary()/SendBinary() hit a
    // real error (as opposed to a receive timeout, which leaves the
    // connection intact).
    bool IsConnected() const { return websocket_ != nullptr && connected_; }

private:
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    HINTERNET websocket_ = nullptr;
    bool connected_ = false;
};

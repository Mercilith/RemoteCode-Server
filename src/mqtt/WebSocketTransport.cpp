#include "WebSocketTransport.h"

#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace {
constexpr wchar_t kUserAgent[] = L"RemoteCode-Server/1.0";
}

WebSocketTransport::~WebSocketTransport() {
    Close();
}

bool WebSocketTransport::Connect(
    const std::wstring& host,
    INTERNET_PORT port,
    const std::wstring& path,
    const std::wstring& subprotocol) {
    session_ = WinHttpOpen(
        kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_ == nullptr) {
        return false;
    }

    connect_ = WinHttpConnect(session_, host.c_str(), port, 0);
    if (connect_ == nullptr) {
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect_, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        return false;
    }

    const std::wstring subprotocolHeader = L"Sec-WebSocket-Protocol: " + subprotocol;
    WinHttpAddRequestHeaders(
        request, subprotocolHeader.c_str(), static_cast<DWORD>(-1),
        WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        WinHttpCloseHandle(request);
        return false;
    }

    if (!WinHttpSendRequest(
            request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(request);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        return false;
    }

    websocket_ = WinHttpWebSocketCompleteUpgrade(request, 0);
    // The request handle's role ends once the upgrade completes — the
    // returned websocket_ handle owns the connection from here.
    WinHttpCloseHandle(request);

    connected_ = websocket_ != nullptr;
    return connected_;
}

void WebSocketTransport::SetReceiveTimeoutMs(DWORD timeoutMs) {
    if (websocket_ == nullptr) {
        return;
    }
    // This SDK doesn't expose the WebSocket-specific receive-timeout
    // option (added in later SDKs); the general receive-timeout option
    // applies to WinHttpWebSocketReceive just the same.
    WinHttpSetOption(websocket_, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
}

bool WebSocketTransport::SendBinary(const uint8_t* data, size_t len) {
    if (websocket_ == nullptr) {
        return false;
    }
    DWORD result = WinHttpWebSocketSend(
        websocket_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
        const_cast<uint8_t*>(data), static_cast<DWORD>(len));
    if (result != NO_ERROR) {
        connected_ = false;
        return false;
    }
    return true;
}

bool WebSocketTransport::ReceiveBinary(std::vector<uint8_t>& outData) {
    if (websocket_ == nullptr) {
        return false;
    }
    outData.clear();

    constexpr DWORD kChunkSize = 4096;
    std::vector<uint8_t> chunk(kChunkSize);

    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType;
        DWORD result = WinHttpWebSocketReceive(
            websocket_, chunk.data(), kChunkSize, &bytesRead, &bufferType);
        if (result != NO_ERROR) {
            // A receive timeout leaves the connection intact — everything
            // else is treated as a real disconnect.
            if (result != ERROR_WINHTTP_TIMEOUT) {
                connected_ = false;
            }
            return false;
        }
        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            connected_ = false;
            return false;
        }

        outData.insert(outData.end(), chunk.begin(), chunk.begin() + bytesRead);

        const bool isFinal = bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                              bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        if (isFinal) {
            return true;
        }
        // Otherwise it's a *_FRAGMENT_BUFFER_TYPE — keep reading until the
        // final fragment of this logical message arrives.
    }
}

void WebSocketTransport::Close() {
    connected_ = false;
    if (websocket_ != nullptr) {
        WinHttpWebSocketClose(websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(websocket_);
        websocket_ = nullptr;
    }
    if (connect_ != nullptr) {
        WinHttpCloseHandle(connect_);
        connect_ = nullptr;
    }
    if (session_ != nullptr) {
        WinHttpCloseHandle(session_);
        session_ = nullptr;
    }
}

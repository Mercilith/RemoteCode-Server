#include "ServiceMain.h"

#include <windows.h>

#include <shlobj.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "../identity/ServerIdentity.h"
#include "../mqtt/MqttClient.h"
#include "../topic/TopicDerivation.h"

namespace fs = std::filesystem;

namespace {

// Must match the service name the Installer registers via CreateServiceW
// (src/elevate/PathAndStartupInstaller.cpp in RemoteCode-Installer).
constexpr wchar_t kServiceName[] = L"RemoteCodeServer";

constexpr wchar_t kMqttHost[] = L"broker.hivemq.com";
constexpr INTERNET_PORT kMqttPort = 8884; // HiveMQ's public broker, MQTT-over-WSS
constexpr wchar_t kMqttWebSocketPath[] = L"/mqtt";
constexpr char kMqttClientId[] = "remotecode-server";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_shutdownEvent = nullptr;
bool g_consoleMode = false;

std::wstring LogFilePath() {
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) ||
        programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return L"";
    }
    std::wstring dir = std::wstring(programData) + L"\\RemoteCode\\Server";
    CoTaskMemFree(programData);

    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir + L"\\service.log";
}

std::wstring TimestampNow() {
    time_t t = time(nullptr);
    tm utc{};
    gmtime_s(&utc, &t);
    wchar_t buffer[32];
    wcsftime(buffer, _countof(buffer), L"%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

void Log(const std::wstring& message) {
    const std::wstring line = TimestampNow() + L" " + message;

    const std::wstring path = LogFilePath();
    if (!path.empty()) {
        std::wofstream file(path.c_str(), std::ios::app);
        if (file.is_open()) {
            file << line << L"\n";
        }
    }
    if (g_consoleMode) {
        std::wcout << line << std::endl;
    }
}

// Topics are lowercase hex (ASCII-only), so a plain widen is safe here —
// no need for a full UTF-8 conversion for this specific string shape.
std::wstring WidenHex(const std::string& hex) {
    return std::wstring(hex.begin(), hex.end());
}

void SetStatus(DWORD state, DWORD exitCode = 0, DWORD waitHint = 0) {
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        (state == SERVICE_START_PENDING) ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    if (g_statusHandle != nullptr) {
        SetServiceStatus(g_statusHandle, &g_status);
    }
}

DWORD WINAPI ControlHandler(DWORD control, DWORD /*eventType*/, LPVOID /*eventData*/, LPVOID /*context*/) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            SetStatus(SERVICE_STOP_PENDING, 0, 3000);
            SetEvent(g_shutdownEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        SetEvent(g_shutdownEvent);
        return TRUE;
    }
    return FALSE;
}

bool ShuttingDown() {
    return WaitForSingleObject(g_shutdownEvent, 0) == WAIT_OBJECT_0;
}

void RunMainLoop() {
    ServerIdentity identity;
    if (!ServerIdentityStore::LoadOrCreate(identity)) {
        Log(L"Failed to load or create the server identity — cannot continue.");
        return;
    }
    Log(L"Server identity ready.");

    while (!ShuttingDown()) {
        MqttClient client;
        Log(L"Connecting to MQTT broker...");
        if (!client.Connect(kMqttHost, kMqttPort, kMqttWebSocketPath, kMqttClientId)) {
            Log(L"MQTT connect failed; retrying in 10s.");
            WaitForSingleObject(g_shutdownEvent, 10000);
            continue;
        }
        Log(L"Connected.");

        int64_t currentEpoch = TopicDerivation::CurrentEpoch(time(nullptr));
        std::string currentTopic = TopicDerivation::DeriveTopicForEpoch(identity.publicKey, currentEpoch);
        if (!client.Subscribe(currentTopic)) {
            Log(L"Subscribe failed; reconnecting.");
            client.Disconnect();
            WaitForSingleObject(g_shutdownEvent, 5000);
            continue;
        }
        Log(L"Subscribed to topic " + WidenHex(currentTopic));

        bool connectionOk = true;
        while (connectionOk && !ShuttingDown()) {
            const int64_t nowEpoch = TopicDerivation::CurrentEpoch(time(nullptr));
            if (nowEpoch != currentEpoch) {
                const std::string newTopic =
                    TopicDerivation::DeriveTopicForEpoch(identity.publicKey, nowEpoch);
                client.Unsubscribe(currentTopic);
                if (client.Subscribe(newTopic)) {
                    Log(L"Rotated to topic " + WidenHex(newTopic));
                    currentEpoch = nowEpoch;
                    currentTopic = newTopic;
                } else {
                    Log(L"Resubscribe after topic rotation failed; reconnecting.");
                    connectionOk = false;
                    break;
                }
            }

            connectionOk = client.PumpOnce(
                [](const std::string& topic, const std::vector<uint8_t>& payload) {
                    Log(L"Message on " + WidenHex(topic) + L": " + std::to_wstring(payload.size()) +
                        L" byte(s).");
                });
        }

        client.Disconnect();
        if (!ShuttingDown()) {
            Log(L"Disconnected; reconnecting in 5s.");
            WaitForSingleObject(g_shutdownEvent, 5000);
        }
    }

    Log(L"Shutting down.");
}

void WINAPI ServiceMainProc(DWORD /*argc*/, LPWSTR* /*argv*/) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    if (g_statusHandle == nullptr) {
        return;
    }

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    SetStatus(SERVICE_START_PENDING, 0, 3000);

    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_shutdownEvent == nullptr) {
        SetStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    SetStatus(SERVICE_RUNNING);
    RunMainLoop();

    CloseHandle(g_shutdownEvent);
    g_shutdownEvent = nullptr;
    SetStatus(SERVICE_STOPPED);
}

} // namespace

int ServiceMain::Run() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMainProc},
        {nullptr, nullptr},
    };

    if (StartServiceCtrlDispatcherW(table)) {
        return 0;
    }

    if (GetLastError() != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        return 1;
    }

    // Not launched by the SCM — run the same loop directly for local
    // debugging instead of requiring an install for every test run.
    g_consoleMode = true;
    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_shutdownEvent == nullptr) {
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    std::wcout << L"Not running under the Service Control Manager — running in console mode. "
                  L"Press Ctrl+C to stop."
               << std::endl;
    RunMainLoop();

    CloseHandle(g_shutdownEvent);
    g_shutdownEvent = nullptr;
    return 0;
}

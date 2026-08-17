#include "ServiceMain.h"

#include <windows.h>

#include <shlobj.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "../orchestrator/Orchestrator.h"

namespace fs = std::filesystem;

namespace {

// Must match the service name the Installer registers via CreateServiceW
// (src/elevate/PathAndStartupInstaller.cpp in RemoteCode-Installer).
constexpr wchar_t kServiceName[] = L"RemoteCodeServer";

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
    // Sibling of the install dir, not the exe's own directory.
    std::wstring dir = std::wstring(programData) + L"\\RemoteCode\\ServerData";
    CoTaskMemFree(programData);

    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir + L"\\service.log";
}

// The service (running as SYSTEM when installed via the SCM) and ad-hoc
// console-mode debug runs (running as the logged-in user) share one log
// file. Whichever identity creates it first can leave the other unable to
// append to it (NTFS ACL inheritance doesn't grant both identities write
// access to a file created by one of them). Resolved once per process:
// probe the primary path, and if it can't be opened for append, fall back
// to a sibling file this process can create fresh rather than losing all
// log output silently.
const std::wstring& ActiveLogPath() {
    static const std::wstring path = [] {
        const std::wstring primary = LogFilePath();
        if (primary.empty()) {
            return primary;
        }
        std::wofstream probe(primary.c_str(), std::ios::app);
        if (probe.is_open()) {
            return primary;
        }
        const size_t slash = primary.find_last_of(L"\\/");
        return primary.substr(0, slash) + L"\\service-current.log";
    }();
    return path;
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

    const std::wstring& path = ActiveLogPath();
    if (!path.empty()) {
        std::wofstream file(path.c_str(), std::ios::app);
        if (file.is_open()) {
            file << line << L"\n";
        }
    }
    if (g_consoleMode) {
        // Narrow, ASCII-only write — log messages are always ASCII (see
        // Orchestrator.cpp's AsciiToWide comment), and wcout on a redirected
        // (non-console) stdout handle can wedge on certain wide characters,
        // silently dropping all further output for the process's lifetime.
        std::string narrow;
        narrow.reserve(line.size());
        for (const wchar_t c : line) {
            narrow.push_back(static_cast<char>(c));
        }
        std::cout << narrow << std::endl;
    }
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
            SetStatus(SERVICE_STOP_PENDING, 0, 5000);
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

void RunMainLoop() {
    Orchestrator orchestrator;
    orchestrator.Run(g_shutdownEvent, Log);
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

    std::cout << "Not running under the Service Control Manager - running in console mode. "
                 "Press Ctrl+C to stop."
              << std::endl;
    RunMainLoop();

    CloseHandle(g_shutdownEvent);
    g_shutdownEvent = nullptr;
    return 0;
}

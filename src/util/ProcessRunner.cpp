#include "ProcessRunner.h"

#include <windows.h>

#include <vector>

bool ProcessRunner::RunCommand(
    const std::wstring& commandLine, const std::wstring& cwd, std::string& outOutput, int& outExitCode) {
    outExitCode = -1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    // Combined stdout+stderr into the same pipe — a nonzero exit's error
    // detail (e.g. gh's "repository not found") is normally on stderr.
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    const BOOL created = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);

    CloseHandle(outWrite);

    if (!created) {
        CloseHandle(outRead);
        return false;
    }

    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(outRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        outOutput.append(buffer, bytesRead);
    }
    CloseHandle(outRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    if (GetExitCodeProcess(pi.hProcess, &exitCode)) {
        outExitCode = static_cast<int>(exitCode);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

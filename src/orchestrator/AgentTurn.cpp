#include "AgentTurn.h"

#include <windows.h>

#include <thread>
#include <vector>

#include "../third_party/json.hpp"

using nlohmann::json;

namespace {

std::wstring ExeDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len == MAX_PATH) {
        return L"";
    }
    const std::wstring path(buffer, len);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

std::wstring ExePath() {
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return len == 0 ? L"" : std::wstring(buffer, len);
}

// Looks for <dir>\worker\node_modules\tsx\dist\cli.mjs, walking up from the
// exe's directory a few levels — covers both the installed layout (worker/
// copied next to the exe by RemoteCode-Installer, task #50) and this dev
// build tree (RemoteCode-Server\build\Release\..\..\worker).
bool FindWorkerEntry(std::wstring& outWorkerDir, std::wstring& outTsxCliPath) {
    std::wstring dir = ExeDirectory();
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
        const std::wstring candidateDir = dir + L"\\worker";
        const std::wstring tsxCli = candidateDir + L"\\node_modules\\tsx\\dist\\cli.mjs";
        if (GetFileAttributesW(tsxCli.c_str()) != INVALID_FILE_ATTRIBUTES) {
            outWorkerDir = candidateDir;
            outTsxCliPath = tsxCli;
            return true;
        }
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            break;
        }
        dir = dir.substr(0, slash);
    }
    return false;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// Runs `commandLine` with `cwd`, writing `stdinData` to its stdin and
// capturing stdout into `outOutput` (stderr is discarded so stray
// tsx/node warnings can't corrupt the JSON we parse from stdout). Returns
// false only if the process could not be created at all.
bool RunProcessCapture(
    const std::wstring& commandLine, const std::wstring& cwd, const std::string& stdinData,
    std::string& outOutput) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr, stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0) || !CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
        return false;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE stderrTarget = CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stderrTarget;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    const BOOL created = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);

    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (stderrTarget != INVALID_HANDLE_VALUE) {
        CloseHandle(stderrTarget);
    }

    if (!created) {
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        return false;
    }

    std::thread writer([stdinWrite, &stdinData]() {
        DWORD written = 0;
        if (!stdinData.empty()) {
            WriteFile(stdinWrite, stdinData.data(), static_cast<DWORD>(stdinData.size()), &written, nullptr);
        }
        CloseHandle(stdinWrite);
    });

    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(stdoutRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        outOutput.append(buffer, bytesRead);
    }
    CloseHandle(stdoutRead);

    writer.join();

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// The worker writes exactly one JSON line as its final act, but tsx/node
// could in principle emit stray lines first — scan from the last
// non-empty line backwards for the first one that parses as JSON.
bool ParseLastJsonLine(const std::string& output, json& outParsed) {
    size_t end = output.size();
    while (end > 0) {
        size_t start = output.rfind('\n', end - 1);
        start = (start == std::string::npos) ? 0 : start + 1;
        const std::string line = output.substr(start, end - start);
        if (!line.empty()) {
            try {
                outParsed = json::parse(line);
                return true;
            } catch (const json::parse_error&) {
                // fall through to the previous line
            }
        }
        if (start == 0) {
            break;
        }
        end = start - 1;
    }
    return false;
}

} // namespace

AgentTurnResult AgentTurn::Run(
    const Agent& agent, const std::vector<Message>& recentMessages, const std::wstring& dbPath,
    const std::string& claudeConfigDir, const std::string& chatId) {
    AgentTurnResult result;

    std::wstring workerDir, tsxCliPath;
    if (!FindWorkerEntry(workerDir, tsxCliPath)) {
        result.error = "worker/ (Node.js Agent SDK subprocess) not found next to the server executable";
        return result;
    }

    json request;
    request["systemPrompt"] = agent.systemPrompt;

    json messages = json::array();
    for (const Message& m : recentMessages) {
        messages.push_back({
            {"senderType", m.senderType},
            {"senderId", m.senderId},
            {"content", m.content},
        });
    }
    request["messages"] = messages;
    request["mcpServerCommand"] = WideToUtf8(ExePath());
    request["mcpServerArgs"] = json::array(
        {"--mcp-server", "--agent-id", agent.id, "--chat-id", chatId, "--db-path", WideToUtf8(dbPath)});
    request["claudeConfigDir"] = claudeConfigDir;

    const std::wstring commandLine = L"node.exe \"" + tsxCliPath + L"\" \"src/index.ts\"";

    std::string output;
    if (!RunProcessCapture(commandLine, workerDir, request.dump(), output)) {
        result.error = "failed to launch worker subprocess (is Node.js installed and on PATH?)";
        return result;
    }

    json parsed;
    if (!ParseLastJsonLine(output, parsed)) {
        result.error = "worker produced no JSON output: " + output;
        return result;
    }

    if (parsed.contains("error")) {
        result.error = parsed["error"].get<std::string>();
        return result;
    }

    result.ok = true;
    result.response = parsed.value("response", "");
    return result;
}

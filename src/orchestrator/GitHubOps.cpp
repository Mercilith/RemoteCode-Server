#include "GitHubOps.h"

#include <windows.h>

#include <fstream>

#include "../util/ProcessRunner.h"

using nlohmann::json;

namespace GitHubOps {
namespace {

// Mirrors WorkspacePr.cpp's QuoteArg/Trim/ExtractPrUrl rather than exporting
// them -- same rationale given there: small enough not to be worth a shared
// header for two translation units, and this module's inputs (repo owner/
// repo strings, titles, comments) go through the identical Windows command-
// line quoting needs.
std::wstring AsciiToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

std::wstring QuoteArg(const std::string& arg) {
    std::wstring escaped;
    escaped.reserve(arg.size() + 2);
    escaped.push_back(L'"');
    for (const char c : arg) {
        if (c == '"') {
            escaped.push_back(L'\\');
        }
        escaped.push_back(static_cast<wchar_t>(c));
    }
    escaped.push_back(L'"');
    return escaped;
}

std::string Trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Same "scan for the last non-blank line" approach as WorkspacePr.cpp's
// ExtractPrUrl -- used here for `gh issue create`'s URL output.
std::string ExtractLastNonBlankLine(const std::string& output) {
    std::string lastNonBlank;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t newlinePos = output.find('\n', pos);
        const std::string line = Trim(output.substr(pos, newlinePos == std::string::npos ? std::string::npos
                                                                                            : newlinePos - pos));
        if (!line.empty()) {
            lastNonBlank = line;
        }
        if (newlinePos == std::string::npos) {
            break;
        }
        pos = newlinePos + 1;
    }
    return lastNonBlank;
}

// Writes `text` to a fresh temp file and returns its path, or an empty
// wstring on failure -- same GetTempPathW/GetTempFileNameW pattern as
// WorkspacePr.cpp's PR-body temp file, reused here for review comments and
// issue bodies (both free-form, possibly multi-line text that would fight
// command-line quoting if passed inline).
std::wstring WriteTempFile(const std::string& text, std::string& outError) {
    wchar_t tempDir[MAX_PATH];
    wchar_t tempFilePath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempDir) == 0 || GetTempFileNameW(tempDir, L"rcgh", 0, tempFilePath) == 0) {
        outError = "failed to create a temp file";
        return L"";
    }
    std::ofstream file(tempFilePath, std::ios::binary);
    if (!file) {
        outError = "failed to write to a temp file";
        DeleteFileW(tempFilePath);
        return L"";
    }
    file << text;
    file.close();
    return std::wstring(tempFilePath);
}

// Runs `commandLine` (gh.exe ...) and returns Result::ok=false with a
// caller-facing message on either launch failure or a nonzero exit code --
// the same two failure modes WorkspacePr::Create distinguishes.
struct RunResult {
    bool ok = false;
    std::string output;
    std::string error;
};

RunResult RunGh(const std::wstring& commandLine) {
    RunResult result;
    std::string output;
    int exitCode = -1;
    if (!ProcessRunner::RunCommand(commandLine, L"", output, exitCode)) {
        result.error = "failed to launch 'gh' (is the gh CLI installed and on PATH?)";
        return result;
    }
    if (exitCode != 0) {
        result.error = "gh command failed: " + output;
        return result;
    }
    result.ok = true;
    result.output = output;
    return result;
}

} // namespace

bool ResolveListState(const std::string& filter, bool forPr, std::string& outState, std::string& outError) {
    const std::string effective = filter.empty() ? "open" : filter;
    if (effective == "open" || effective == "closed" || effective == "all") {
        outState = effective;
        return true;
    }
    if (forPr && effective == "merged") {
        outState = effective;
        return true;
    }
    outError = forPr ? "status must be one of 'open', 'closed', 'merged', 'all' (got '" + filter + "')"
                      : "status must be one of 'open', 'closed', 'all' (got '" + filter + "')";
    return false;
}

bool ValidateSetPrStatus(const std::string& status, std::string& outError) {
    if (status == "merged") {
        outError = "set_pr_status does not accept 'merged' -- use merge_pull_request to merge a PR instead";
        return false;
    }
    if (status == "open" || status == "closed" || status == "approved" || status == "changes-requested") {
        return true;
    }
    outError = "status must be one of 'open', 'closed', 'approved', 'changes-requested' (got '" + status + "')";
    return false;
}

bool ValidateSetIssueStatus(const std::string& status, std::string& outError) {
    if (status == "open" || status == "closed") {
        return true;
    }
    outError = "status must be one of 'open', 'closed' (got '" + status + "')";
    return false;
}

bool ValidateMergeMethod(const std::string& method, std::string& outMethod, std::string& outError) {
    const std::string effective = method.empty() ? "merge" : method;
    if (effective == "merge" || effective == "squash" || effective == "rebase") {
        outMethod = effective;
        return true;
    }
    outError = "merge_method must be one of 'merge', 'squash', 'rebase' (got '" + method + "')";
    return false;
}

Result ListPullRequests(const std::string& ownerRepo, const std::string& state) {
    Result result;
    const std::wstring commandLine = L"gh.exe pr list --repo " + QuoteArg(ownerRepo) + L" --state " +
                                      QuoteArg(state) + L" --limit 200 --json number,title,url,state,headRefName";
    const RunResult run = RunGh(commandLine);
    if (!run.ok) {
        result.error = run.error;
        return result;
    }
    try {
        result.data = json::parse(run.output);
    } catch (const json::parse_error&) {
        result.error = "gh pr list returned output that could not be parsed as JSON: " + run.output;
        return result;
    }
    result.ok = true;
    return result;
}

Result GetPrStatus(const std::string& ownerRepo, int prNumber) {
    Result result;
    const std::wstring commandLine = L"gh.exe pr view " + std::to_wstring(prNumber) + L" --repo " +
                                      QuoteArg(ownerRepo) +
                                      L" --json number,title,url,state,mergeable,reviewDecision";
    const RunResult run = RunGh(commandLine);
    if (!run.ok) {
        result.error = run.error;
        return result;
    }
    try {
        result.data = json::parse(run.output);
    } catch (const json::parse_error&) {
        result.error = "gh pr view returned output that could not be parsed as JSON: " + run.output;
        return result;
    }
    result.ok = true;
    return result;
}

Result SetPrStatus(
    const std::string& ownerRepo, int prNumber, const std::string& status, const std::string& comment) {
    Result result;
    const std::wstring numberArg = std::to_wstring(prNumber);

    if (status == "open") {
        const RunResult run = RunGh(L"gh.exe pr reopen " + numberArg + L" --repo " + QuoteArg(ownerRepo));
        if (!run.ok) {
            result.error = run.error;
            return result;
        }
        result.ok = true;
        return result;
    }
    if (status == "closed") {
        const RunResult run = RunGh(L"gh.exe pr close " + numberArg + L" --repo " + QuoteArg(ownerRepo));
        if (!run.ok) {
            result.error = run.error;
            return result;
        }
        result.ok = true;
        return result;
    }

    // approved / changes-requested both go through `gh pr review`.
    // --request-changes requires a body; --approve accepts one optionally.
    // A placeholder is used for changes-requested when the caller supplied
    // no comment, since gh errors out on an empty --body-file otherwise --
    // see the design note in mcp/Tools.cpp's SetPrStatusTool for why an
    // optional `comment` arg was added rather than forcing every caller to
    // always supply one.
    std::string body = comment;
    if (status == "changes-requested" && body.empty()) {
        body = "Changes requested via RemoteCode.";
    }

    std::wstring reviewFlag = (status == "approved") ? L" --approve" : L" --request-changes";
    std::wstring commandLine = L"gh.exe pr review " + numberArg + L" --repo " + QuoteArg(ownerRepo) + reviewFlag;

    std::wstring tempFilePath;
    if (!body.empty()) {
        std::string tempError;
        tempFilePath = WriteTempFile(body, tempError);
        if (tempFilePath.empty()) {
            result.error = tempError;
            return result;
        }
        commandLine += L" --body-file \"" + tempFilePath + L"\"";
    }

    const RunResult run = RunGh(commandLine);
    if (!tempFilePath.empty()) {
        DeleteFileW(tempFilePath.c_str());
    }
    if (!run.ok) {
        result.error = run.error;
        return result;
    }
    result.ok = true;
    return result;
}

Result MergePullRequest(const std::string& ownerRepo, int prNumber, const std::string& mergeMethod) {
    Result result;
    const std::wstring methodFlag = L" --" + AsciiToWide(mergeMethod);
    const std::wstring commandLine =
        L"gh.exe pr merge " + std::to_wstring(prNumber) + L" --repo " + QuoteArg(ownerRepo) + methodFlag;
    const RunResult run = RunGh(commandLine);
    if (!run.ok) {
        result.error = run.error;
        return result;
    }
    result.ok = true;
    result.text = Trim(run.output);
    return result;
}

Result ListIssues(const std::string& ownerRepo, const std::string& state) {
    Result result;
    const std::wstring commandLine = L"gh.exe issue list --repo " + QuoteArg(ownerRepo) + L" --state " +
                                      QuoteArg(state) + L" --limit 200 --json number,title,url,state";
    const RunResult run = RunGh(commandLine);
    if (!run.ok) {
        result.error = run.error;
        return result;
    }
    try {
        result.data = json::parse(run.output);
    } catch (const json::parse_error&) {
        result.error = "gh issue list returned output that could not be parsed as JSON: " + run.output;
        return result;
    }
    result.ok = true;
    return result;
}

Result CreateIssue(const std::string& ownerRepo, const std::string& title, const std::string& body) {
    Result result;
    std::string tempError;
    const std::wstring tempFilePath = WriteTempFile(body, tempError);
    if (tempFilePath.empty()) {
        result.error = tempError;
        return result;
    }
    const std::wstring commandLine = L"gh.exe issue create --repo " + QuoteArg(ownerRepo) + L" --title " +
                                      QuoteArg(title) + L" --body-file \"" + tempFilePath + L"\"";
    const RunResult run = RunGh(commandLine);
    DeleteFileW(tempFilePath.c_str());
    if (!run.ok) {
        result.error = run.error;
        return result;
    }
    result.ok = true;
    result.text = ExtractLastNonBlankLine(run.output);
    return result;
}

Result SetIssueStatus(const std::string& ownerRepo, int issueNumber, const std::string& status) {
    Result result;
    const std::wstring numberArg = std::to_wstring(issueNumber);
    const std::wstring subcommand = (status == "open") ? L"reopen" : L"close";
    const RunResult run = RunGh(L"gh.exe issue " + subcommand + L" " + numberArg + L" --repo " + QuoteArg(ownerRepo));
    if (!run.ok) {
        result.error = run.error;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace GitHubOps
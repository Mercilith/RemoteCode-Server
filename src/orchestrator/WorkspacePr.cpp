#include "WorkspacePr.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "../util/ProcessRunner.h"
#include "WorkspaceCreator.h"

namespace fs = std::filesystem;

namespace WorkspacePr {
namespace {

// Ids/paths here are always ASCII (generated identifiers, or agent-supplied
// text re-encoded byte-for-byte) — same rationale as WorkspaceCreator.cpp's
// AsciiToWide/WideToAscii, which this mirrors rather than exporting (small
// enough not to be worth a shared header for two translation units).
std::wstring AsciiToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

std::string WideToAscii(const std::wstring& s) {
    std::string result;
    result.reserve(s.size());
    for (const wchar_t c : s) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

// Wraps `arg` in double quotes for a CreateProcessW-style single command
// line, escaping any embedded double quotes with a backslash. Not a fully
// general Windows command-line quoter (doesn't handle a literal trailing
// backslash immediately before the closing quote) — good enough for the
// short, human-written title text this is used for, matching this
// codebase's existing informal level of quoting elsewhere (e.g.
// Orchestrator::RunRepoOnboarding's gh repo clone call).
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

// `gh pr create` (non-interactive, given --title/--body-file/--base/--head)
// prints the new PR's URL as the last non-blank line of stdout on success —
// scan from the end rather than assuming it's the only line, since gh can
// print informational lines before it (e.g. about the branch being pushed).
std::string ExtractPrUrl(const std::string& output) {
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

} // namespace

Result Create(
    const std::string& workspaceId, const std::string& repoId, const std::string& title, const std::string& body,
    const std::string& baseBranch) {
    Result result;

    const std::wstring worktreePath = WorkspaceCreator::RepoWorktreePath(workspaceId, repoId);
    if (worktreePath.empty()) {
        result.error = "could not resolve %ProgramData% to locate the worktree";
        return result;
    }
    std::error_code existsEc;
    if (!fs::exists(fs::path(worktreePath) / L".git", existsEc)) {
        result.error = "no worktree exists yet for repo '" + repoId + "' in workspace '" + workspaceId +
                        "' — has this workspace been created?";
        return result;
    }

    const std::string branch = WorkspaceCreator::RepoWorktreeBranch(workspaceId, repoId);
    const std::string effectiveBase = baseBranch.empty() ? "main" : baseBranch;

    // Push the branch first — gh pr create needs it to already exist on the
    // remote (it doesn't push for you).
    {
        const std::wstring pushCommandLine = L"git push -u origin " + QuoteArg(branch);
        std::string output;
        int exitCode = -1;
        if (!ProcessRunner::RunCommand(pushCommandLine, worktreePath, output, exitCode)) {
            result.error = "failed to launch 'git push' (is git installed and on PATH?)";
            return result;
        }
        if (exitCode != 0) {
            result.error = "'git push' failed: " + output;
            return result;
        }
    }

    // gh's --body-file avoids fighting Windows command-line quoting over a
    // free-form, possibly multi-line/multi-paragraph PR body — title stays
    // a normal quoted argument since it's expected to be a short one-liner.
    wchar_t tempDir[MAX_PATH];
    wchar_t tempFilePath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempDir) == 0 ||
        GetTempFileNameW(tempDir, L"rcpr", 0, tempFilePath) == 0) {
        result.error = "failed to create a temp file for the PR body";
        return result;
    }
    {
        std::ofstream bodyFile(tempFilePath, std::ios::binary);
        if (!bodyFile) {
            result.error = "failed to write the PR body to a temp file";
            DeleteFileW(tempFilePath);
            return result;
        }
        bodyFile << body;
    }

    const std::wstring prCommandLine = L"gh.exe pr create --title " + QuoteArg(title) + L" --body-file \"" +
                                        std::wstring(tempFilePath) + L"\" --base " + QuoteArg(effectiveBase) +
                                        L" --head " + QuoteArg(branch);
    std::string output;
    int exitCode = -1;
    const bool launched = ProcessRunner::RunCommand(prCommandLine, worktreePath, output, exitCode);
    DeleteFileW(tempFilePath);

    if (!launched) {
        result.error = "failed to launch 'gh pr create' (is the gh CLI installed and on PATH?)";
        return result;
    }
    if (exitCode != 0) {
        result.error = "'gh pr create' failed: " + output;
        return result;
    }

    result.ok = true;
    result.prUrl = ExtractPrUrl(output);
    return result;
}

} // namespace WorkspacePr

#include "RepoImport.h"

#include <shlobj.h>

#include <ctime>
#include <filesystem>

#include "../util/GitHubRepo.h"
#include "../util/ProcessRunner.h"

namespace fs = std::filesystem;

namespace RepoImport {
namespace {

// Mirrors Orchestrator.cpp's AsciiToWide/WideToUtf8 and
// WorkspaceCreator.cpp's RepoWorktreePath — same small-enough-not-to-share
// rationale given there.
std::wstring AsciiToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// Mirrors WorkspacePr.cpp/GitHubOps.cpp's QuoteArg rather than exporting it
// -- same small-enough-not-to-share rationale given there. `name` below is
// LLM/agent-supplied via the create_repo tool with no charset validation, so
// unlike repoDisplayName (built from org/repo strings GitHubRepo::ParseGitHubUrl
// already validated), it genuinely needs this: a literal `"..."`-wrapped
// argument with no escaping lets an embedded `"` in `name` break out of its
// quoted segment and inject extra argv tokens into the gh.exe invocation.
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

std::string Trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Same "scan for the last non-blank line" approach as WorkspacePr.cpp's
// ExtractPrUrl / GitHubOps.cpp's ExtractLastNonBlankLine — `gh repo create`
// prints the new repo's URL as the last non-blank line of stdout on success.
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

// Identical to Orchestrator.cpp's (anonymous-namespace, not exported)
// RepoLocalPath — %ProgramData%\RemoteCode\Repos\<repoId>, the exact same
// deterministic path AddRepo/RunRepoOnboarding use, so a repo imported via
// this tool and one added via /add-repo end up in the same place (and the
// idempotence check below actually means something).
std::wstring RepoLocalPath(const std::string& repoId) {
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) || programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return L"";
    }
    const std::wstring dir = std::wstring(programData) + L"\\RemoteCode\\Repos\\" + AsciiToWide(repoId);
    CoTaskMemFree(programData);

    std::error_code ec;
    fs::create_directories(fs::path(dir).parent_path(), ec);
    return dir;
}

} // namespace

Result Import(RepoStore& repoStore, const std::string& githubUrl, const std::string& notes) {
    Result result;

    std::string org, repoName;
    if (!GitHubRepo::ParseGitHubUrl(githubUrl, org, repoName)) {
        result.error = "could not parse '" + githubUrl +
                        "' as a GitHub repo (expected https://github.com/org/repo, "
                        "git@github.com:org/repo.git, or org/repo)";
        return result;
    }
    const std::string repoId = GitHubRepo::RepoId(org, repoName);
    const std::string repoDisplayName = org + "/" + repoName;

    Repo existing;
    if (repoStore.Get(repoId, existing)) {
        // Idempotent, same rule as Orchestrator::AddRepo — importing the
        // same repo twice is a no-op past the first time.
        result.ok = true;
        result.repoId = existing.id;
        result.githubUrl = existing.githubUrl;
        return result;
    }

    const std::wstring localPath = RepoLocalPath(repoId);
    if (localPath.empty()) {
        result.error = "could not resolve %ProgramData% to determine the clone destination";
        return result;
    }

    Repo repo;
    repo.id = repoId;
    repo.githubUrl = githubUrl;
    repo.localPath = WideToUtf8(localPath);
    repo.status = "cloning";
    repo.notes = notes;
    repo.createdAt = static_cast<int64_t>(time(nullptr));
    repo.updatedAt = repo.createdAt;
    if (!repoStore.Create(repo)) {
        result.error = "failed to save the new repo record";
        return result;
    }

    // Synchronous — unlike Orchestrator::AddRepo, which detaches a
    // background thread since it has a whole onboarding pipeline to run
    // afterward, this tool call has nothing further to do once the clone
    // finishes, so there's no reason to return before it's actually done.
    const std::wstring commandLine =
        L"gh.exe repo clone " + QuoteArg(repoDisplayName) + L" \"" + localPath + L"\"";
    std::string output;
    int exitCode = -1;
    if (!ProcessRunner::RunCommand(commandLine, L"", output, exitCode)) {
        const std::string error = "failed to launch 'gh repo clone' (is the gh CLI installed and on PATH?)";
        repoStore.SetError(repoId, error, static_cast<int64_t>(time(nullptr)));
        result.error = error;
        return result;
    }
    if (exitCode != 0) {
        repoStore.SetError(repoId, output, static_cast<int64_t>(time(nullptr)));
        result.error = "'gh repo clone' failed: " + output;
        return result;
    }

    repoStore.SetStatus(repoId, "ready", static_cast<int64_t>(time(nullptr)));
    result.ok = true;
    result.repoId = repoId;
    result.githubUrl = githubUrl;
    return result;
}

Result CreateAndImport(RepoStore& repoStore, const std::string& name, const std::string& notes) {
    Result result;
    if (name.empty()) {
        result.error = "create_repo requires a non-empty 'name'";
        return result;
    }

    const std::wstring commandLine = L"gh.exe repo create " + QuoteArg(name) + L" --private";
    std::string output;
    int exitCode = -1;
    if (!ProcessRunner::RunCommand(commandLine, L"", output, exitCode)) {
        result.error = "failed to launch 'gh repo create' (is the gh CLI installed and on PATH?)";
        return result;
    }
    if (exitCode != 0) {
        result.error = "'gh repo create' failed: " + output;
        return result;
    }

    const std::string newRepoUrl = ExtractLastNonBlankLine(output);
    if (newRepoUrl.empty()) {
        result.error = "'gh repo create' succeeded but its output did not contain the new repo's URL: " + output;
        return result;
    }

    return Import(repoStore, newRepoUrl, notes);
}

} // namespace RepoImport

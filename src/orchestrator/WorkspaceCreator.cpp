#include "WorkspaceCreator.h"

#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "../third_party/json.hpp"
#include "../util/GitHubRepo.h"
#include "../util/ProcessRunner.h"
#include "../util/Text.h"

using nlohmann::json;

namespace fs = std::filesystem;

namespace WorkspaceCreator {
namespace {

// IDs/paths built here are always ASCII (generated identifiers) — a
// byte-for-byte widen is exact, same rationale as Orchestrator.cpp's
// AsciiToWide.
std::wstring AsciiToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// Inverse of AsciiToWide — safe here because every wstring this file narrows
// (worktree paths built from %ProgramData% + ASCII generated ids) is ASCII
// by construction, same rationale AsciiToWide documents. Explicit
// static_cast<char> per character avoids a narrowing-conversion warning
// under /W4 (matches main.cpp's NarrowAscii).
std::string WideToAscii(const std::wstring& s) {
    std::string result;
    result.reserve(s.size());
    for (const wchar_t c : s) {
        result.push_back(static_cast<char>(c));
    }
    return result;
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string lowerHaystack = ToLower(haystack);
    const std::string lowerNeedle = ToLower(needle);
    return lowerHaystack.find(lowerNeedle) != std::string::npos;
}

// %ProgramData%\RemoteCode\Workspaces\<workspaceId>\<repoId> — mirrors
// Orchestrator.cpp's RepoLocalPath (%ProgramData%\RemoteCode\Repos\<repoId>)
// convention exactly, one directory level deeper for the workspace id.
// Returns an empty wstring if %ProgramData% can't be resolved.
std::wstring WorkspaceRepoPath(const std::string& workspaceId, const std::string& repoId) {
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) ||
        programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return L"";
    }
    const std::wstring dir = std::wstring(programData) + L"\\RemoteCode\\Workspaces\\" +
                              AsciiToWide(workspaceId) + L"\\" + AsciiToWide(repoId);
    CoTaskMemFree(programData);

    std::error_code ec;
    fs::create_directories(fs::path(dir).parent_path(), ec); // ensure ...\Workspaces\<id>\ exists
    return dir;
}

// Case-insensitive match of `token` against a repo's id, or the org/repo or
// bare repo name parsed out of its githubUrl — same spirit as
// Orchestrator::ResolveActiveAgentByNameOrId's id-or-slugified-name rule.
bool RepoMatchesToken(const Repo& repo, const std::string& lowerToken) {
    if (ToLower(repo.id) == lowerToken) {
        return true;
    }
    std::string org, name;
    if (GitHubRepo::ParseGitHubUrl(repo.githubUrl, org, name)) {
        if (ToLower(name) == lowerToken) {
            return true;
        }
        if (ToLower(org + "/" + name) == lowerToken) {
            return true;
        }
    }
    return false;
}

// --- Dependency heuristic (v1) ---------------------------------------------
// This is deliberately a best-effort text search, NOT a real dependency
// graph: there is no manifest format shared across every kind of repo this
// system might import (a Dart package, a .NET project, a CMake C++ project,
// ...), so instead of parsing any one of them properly, this just greps a
// short list of common manifest filenames at the repo's top level for any
// OTHER already-imported repo's name (or full "org/repo" slug, or literal
// githubUrl) appearing as a substring. A hit pulls that repo into the
// workspace automatically, on the theory that "my manifest mentions your
// repo name" is a reasonable (if noisy) signal of "I depend on you" when the
// two repos are both things Cardon has already imported into RemoteCode.
// False positives are possible (a coincidental name match) and false
// negatives are common (anything not in one of these files, or expressed as
// an opaque registry package name rather than the repo's own name/URL, is
// invisible to this). Runs to a small fixpoint so a one-level-removed
// dependency (A depends on B, B depends on C) gets pulled in too, capped so
// a pathological manifest can't spin forever.
constexpr const char* kManifestFilenames[] = {
    "package.json", "pubspec.yaml", "pubspec.yml", "CMakeLists.txt",
};
constexpr int kDependencyFixpointCap = 5;

std::vector<std::string> FindManifestFiles(const fs::path& repoRoot) {
    std::vector<std::string> found;
    std::error_code ec;
    if (!fs::exists(repoRoot, ec)) {
        return found;
    }
    for (const char* name : kManifestFilenames) {
        const fs::path candidate = repoRoot / name;
        if (fs::exists(candidate, ec)) {
            found.push_back(candidate.string());
        }
    }
    // *.csproj — filename varies per project, so this one needs a directory
    // scan instead of a fixed name. Top-level only, same as everything else
    // here — this is a heuristic, not a recursive project-graph walk.
    for (const auto& entry : fs::directory_iterator(repoRoot, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".csproj") {
            found.push_back(entry.path().string());
        }
    }
    return found;
}

std::string ReadFileBestEffort(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

// Expands `resolved` (already-validated repo ids) in place with any other
// known repo whose name/slug/URL is mentioned in one of `resolved`'s own
// manifest files, to a small fixpoint. See the heuristic comment above.
void ExpandWithHeuristicDependencies(
    std::vector<Repo>& resolved, const std::vector<Repo>& allRepos) {
    std::unordered_set<std::string> includedIds;
    for (const Repo& r : resolved) {
        includedIds.insert(r.id);
    }

    for (int pass = 0; pass < kDependencyFixpointCap; ++pass) {
        bool addedAny = false;

        // Scan every repo currently in the set (including ones added by a
        // previous pass) — snapshot the ids to scan up front since
        // `resolved` may grow inside this loop.
        std::vector<Repo> toScan = resolved;
        for (const Repo& repo : toScan) {
            if (repo.localPath.empty()) {
                continue;
            }
            for (const std::string& manifestPath : FindManifestFiles(fs::path(AsciiToWide(repo.localPath)))) {
                const std::string manifestContent = ReadFileBestEffort(manifestPath);
                if (manifestContent.empty()) {
                    continue;
                }
                for (const Repo& candidate : allRepos) {
                    if (includedIds.count(candidate.id) != 0) {
                        continue; // already in the workspace
                    }
                    std::string org, name;
                    const bool parsed = GitHubRepo::ParseGitHubUrl(candidate.githubUrl, org, name);
                    const bool matches = ContainsCaseInsensitive(manifestContent, candidate.githubUrl) ||
                                         (parsed && ContainsCaseInsensitive(manifestContent, name)) ||
                                         (parsed && ContainsCaseInsensitive(manifestContent, org + "/" + name));
                    if (matches) {
                        resolved.push_back(candidate);
                        includedIds.insert(candidate.id);
                        addedAny = true;
                    }
                }
            }
        }

        if (!addedAny) {
            break;
        }
    }
}

// Idempotent: if the target worktree directory already looks set up (has a
// .git file/dir — `git worktree add` creates a .git FILE, not a directory,
// pointing back at the main clone's gitdir), this is treated as "already
// done" rather than re-running (and failing) `git worktree add` — see
// WorkspaceCreator.h's comment on why CreateWorkspace needs to tolerate
// being re-invoked for the same workspace id.
bool AddWorktree(const Repo& repo, const std::string& workspaceId, std::string& outPath, std::string& outError) {
    const std::wstring targetPath = WorkspaceRepoPath(workspaceId, repo.id);
    if (targetPath.empty()) {
        outError = "could not resolve %ProgramData% to determine the worktree destination";
        return false;
    }
    outPath = WideToAscii(targetPath);

    std::error_code ec;
    if (fs::exists(fs::path(targetPath) / L".git", ec)) {
        return true; // already set up — idempotent no-op
    }

    const std::string branch = "workspace/" + workspaceId + "/" + repo.id;
    const std::wstring commandLine = L"git worktree add \"" + targetPath + L"\" -b \"" +
                                      AsciiToWide(branch) + L"\"";

    std::string output;
    int exitCode = -1;
    if (!ProcessRunner::RunCommand(commandLine, AsciiToWide(repo.localPath), output, exitCode)) {
        outError = "failed to launch 'git worktree add' for repo '" + repo.id +
                   "' (is git installed and on PATH?)";
        return false;
    }
    if (exitCode != 0) {
        outError = "'git worktree add' failed for repo '" + repo.id + "': " + output;
        return false;
    }
    return true;
}

} // namespace

Result Create(
    RepoStore& repoStore, WorkspaceStore& workspaceStore, ChatStore& chatStore,
    const std::vector<std::string>& repoIdsOrNames, const std::string& title,
    const std::string& requestedByAgentId) {
    Result result;

    if (repoIdsOrNames.empty()) {
        result.error = "create_workspace requires at least one repo id or name";
        return result;
    }

    const std::vector<Repo> allRepos = repoStore.ListAll();

    // Step 1: resolve each explicitly requested token to an already-imported,
    // actually-cloned repo. Fail clearly (the whole call) if any token
    // doesn't resolve, rather than silently dropping it.
    std::vector<Repo> resolved;
    std::vector<std::string> unresolvedTokens;
    for (const std::string& token : repoIdsOrNames) {
        const std::string lowerToken = ToLower(token);
        const Repo* match = nullptr;
        for (const Repo& candidate : allRepos) {
            if (RepoMatchesToken(candidate, lowerToken)) {
                match = &candidate;
                break;
            }
        }
        if (match == nullptr) {
            unresolvedTokens.push_back(token);
            continue;
        }
        // "already-imported" means it has actually finished cloning — same
        // bar RunRepoOnboarding uses ("ready") through to "active"; "cloning"
        // and "failed" are explicitly not far enough along.
        const bool clonedFarEnough =
            !match->localPath.empty() && (match->status == "ready" || match->status == "onboarding" ||
                                           match->status == "active");
        if (!clonedFarEnough) {
            result.error = "repo '" + token + "' has no local clone yet (status: '" + match->status + "')";
            return result;
        }
        const bool alreadyIncluded =
            std::find_if(resolved.begin(), resolved.end(), [&](const Repo& r) { return r.id == match->id; }) !=
            resolved.end();
        if (!alreadyIncluded) {
            resolved.push_back(*match);
        }
    }

    if (!unresolvedTokens.empty()) {
        std::string joined;
        for (size_t i = 0; i < unresolvedTokens.size(); ++i) {
            if (i > 0) {
                joined += ", ";
            }
            joined += unresolvedTokens[i];
        }
        result.error = "unknown repo(s) (not imported, or no match by id/name): " + joined;
        return result;
    }

    // Step 2: best-effort dependency pull-in — see the comment on
    // ExpandWithHeuristicDependencies above.
    ExpandWithHeuristicDependencies(resolved, allRepos);

    const int64_t now = static_cast<int64_t>(time(nullptr));
    const std::string workspaceId = "workspace-" + Slugify(title.empty() ? "workspace" : title) + "-" +
                                     std::to_string(now);
    const std::string chatId = "workspace-chat-" + workspaceId;

    // Step 4 (spec numbering): create a worktree per resolved repo. Every
    // one of these is a plain subprocess/filesystem operation — never an
    // agent turn, matching this codebase's hard rule that repo/worktree work
    // is always programmatic.
    std::vector<std::string> repoIds;
    std::vector<std::string> worktreePaths;
    for (const Repo& repo : resolved) {
        std::string path;
        std::string worktreeError;
        if (!AddWorktree(repo, workspaceId, path, worktreeError)) {
            result.error = worktreeError;
            return result;
        }
        repoIds.push_back(repo.id);
        worktreePaths.push_back(path);
    }

    // Step 3: persist the workspace row. Status starts at "creating" and is
    // advanced to "ready" below once the DB-side setup (chat + participants
    // + initial message) also succeeds — Discord category/channel creation
    // is a separate step neither this function nor its "ready" status covers
    // (see WorkspaceCreator.h's header comment).
    Workspace workspace;
    workspace.id = workspaceId;
    workspace.title = title;
    workspace.repoIdsJson = json(repoIds).dump();
    workspace.chatId = chatId;
    workspace.createdBy = requestedByAgentId.empty() ? "user" : requestedByAgentId;
    workspace.status = "creating";
    workspace.createdAt = now;
    workspace.updatedAt = now;
    if (!workspaceStore.Create(workspace)) {
        result.error = "failed to save the new workspace record";
        return result;
    }

    // Step 6: create the initial conversation chat. discordChannelId left
    // empty on purpose — created lazily (see this file's header comment).
    Chat chat;
    chat.id = chatId;
    chat.title = title.empty() ? workspaceId : title;
    chat.createdBy = workspace.createdBy;
    chat.status = "active";
    chat.createdAt = now;
    if (!chatStore.CreateChat(chat)) {
        workspaceStore.SetError(workspaceId, "failed to create the workspace's initial chat", now);
        result.error = "failed to create the workspace's initial chat";
        return result;
    }

    // Each involved repo's dedicated agent joins as a 'listening' participant
    // — full context, but (per Orchestrator::HandleIncomingMessage's
    // mode-aware dispatch, unchanged by this feature) only actually gets a
    // turn once explicitly @-tagged.
    std::ostringstream summary;
    summary << "**Workspace created: " << (title.empty() ? workspaceId : title) << "**\n"
            << "Repos and worktree paths:\n";
    for (size_t i = 0; i < repoIds.size(); ++i) {
        summary << "- " << repoIds[i] << " -> " << worktreePaths[i] << "\n";
    }
    for (size_t i = 0; i < resolved.size(); ++i) {
        const Repo& repo = resolved[i];
        if (!repo.agentId.empty()) {
            chatStore.AddParticipant(chatId, "agent", repo.agentId);
            chatStore.SetParticipantMode(chatId, "agent", repo.agentId, ParticipantMode::kListening);
            summary << "Added agent '" << repo.agentId << "' (listening mode) for repo '" << repo.id << "'.\n";
        }
    }

    Message initialMessage;
    initialMessage.chatId = chatId;
    initialMessage.senderType = "system";
    initialMessage.senderId = "system";
    initialMessage.type = "system_event";
    initialMessage.content = summary.str();
    initialMessage.createdAt = now;
    chatStore.InsertMessage(initialMessage);

    workspaceStore.SetStatus(workspaceId, "ready", static_cast<int64_t>(time(nullptr)));

    result.ok = true;
    result.workspaceId = workspaceId;
    result.chatId = chatId;
    result.title = title;
    result.repoIds = repoIds;
    result.worktreePaths = worktreePaths;
    return result;
}

} // namespace WorkspaceCreator

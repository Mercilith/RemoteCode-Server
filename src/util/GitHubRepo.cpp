#include "GitHubRepo.h"

#include "Text.h"

namespace {

std::string StripTrailingGitSuffix(std::string s) {
    constexpr const char* kGitSuffix = ".git";
    constexpr size_t kGitSuffixLen = 4;
    if (s.size() > kGitSuffixLen && s.compare(s.size() - kGitSuffixLen, kGitSuffixLen, kGitSuffix) == 0) {
        s.resize(s.size() - kGitSuffixLen);
    }
    return s;
}

std::string StripTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

// Splits "org/repo" (no extra slashes, both non-empty) into its two parts.
// Returns false for anything else (empty, missing a slash, extra segments).
bool SplitOrgRepo(const std::string& path, std::string& outOrg, std::string& outRepo) {
    const size_t slash = path.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    const std::string org = path.substr(0, slash);
    const std::string rest = path.substr(slash + 1);
    if (rest.find('/') != std::string::npos) {
        return false; // extra path segment beyond org/repo
    }
    if (org.empty() || rest.empty()) {
        return false;
    }
    outOrg = org;
    outRepo = rest;
    return true;
}

} // namespace

bool GitHubRepo::ParseGitHubUrl(const std::string& input, std::string& outOrg, std::string& outRepo) {
    std::string s = input;
    // Trim surrounding whitespace — pasted links often carry it.
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return false;
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(begin, end - begin + 1);

    constexpr const char* kHttpsPrefix = "https://github.com/";
    constexpr const char* kHttpPrefix = "http://github.com/";
    constexpr const char* kSshPrefix = "git@github.com:";

    std::string path;
    if (s.rfind(kHttpsPrefix, 0) == 0) {
        path = s.substr(std::string(kHttpsPrefix).size());
    } else if (s.rfind(kHttpPrefix, 0) == 0) {
        path = s.substr(std::string(kHttpPrefix).size());
    } else if (s.rfind(kSshPrefix, 0) == 0) {
        path = s.substr(std::string(kSshPrefix).size());
    } else if (s.find("://") != std::string::npos || s.find('@') != std::string::npos) {
        // Some other host/scheme (not github.com) — reject rather than
        // silently mis-parsing it as a bare org/repo.
        return false;
    } else {
        // Bare "org/repo" form.
        path = s;
    }

    path = StripTrailingSlash(path);
    path = StripTrailingGitSuffix(path);

    return SplitOrgRepo(path, outOrg, outRepo);
}

std::string GitHubRepo::RepoId(const std::string& org, const std::string& repo) {
    return Slugify(org) + "__" + Slugify(repo);
}

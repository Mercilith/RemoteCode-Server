#include "ServerConfig.h"

#include <windows.h>

#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../third_party/json.hpp"
#include "Secrets.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::wstring ConfigDir() {
    PWSTR programData = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData);
    if (FAILED(hr) || programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return L"";
    }
    std::wstring dir = std::wstring(programData) + L"\\RemoteCode\\ServerData";
    CoTaskMemFree(programData);
    return dir;
}

} // namespace

std::wstring ServerConfigStore::ConfigFilePath() {
    const std::wstring dir = ConfigDir();
    if (dir.empty()) {
        return L"";
    }
    return dir + L"\\config.json";
}

ServerConfig ServerConfigStore::Load() {
    ServerConfig config;

    const std::wstring path = ConfigFilePath();
    if (path.empty()) {
        return config;
    }

    std::ifstream file(path.c_str());
    if (!file.is_open()) {
        return config; // no config file yet — not an error, just unconfigured
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    json parsed;
    try {
        parsed = json::parse(buffer.str());
    } catch (const json::parse_error&) {
        return config;
    }

    config.discordGuildId = parsed.value("discord_guild_id", "");
    config.claudeConfigDir = parsed.value("claude_config_dir", "");

    bool resealNeeded = false;
    if (parsed.contains("discord_bot_token_encrypted")) {
        const std::string decrypted = Secrets::Unprotect(parsed["discord_bot_token_encrypted"].get<std::string>());
        if (!decrypted.empty()) {
            config.discordBotToken = decrypted;
        }
    } else if (parsed.contains("discord_bot_token")) {
        // First-run plaintext form — use it now, then reseal below so it
        // doesn't sit in plaintext on disk going forward.
        config.discordBotToken = parsed["discord_bot_token"].get<std::string>();
        resealNeeded = !config.discordBotToken.empty();
    }

    config.valid = !config.discordBotToken.empty();

    if (resealNeeded) {
        Save(config);
    }

    return config;
}

bool ServerConfigStore::Save(const ServerConfig& config) {
    const std::wstring dir = ConfigDir();
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return false;
    }

    json out;
    out["discord_guild_id"] = config.discordGuildId;
    out["claude_config_dir"] = config.claudeConfigDir;

    if (!config.discordBotToken.empty()) {
        const std::string protectedToken = Secrets::Protect(config.discordBotToken);
        if (protectedToken.empty()) {
            return false;
        }
        out["discord_bot_token_encrypted"] = protectedToken;
    }

    const std::wstring path = ConfigFilePath();
    std::ofstream file(path.c_str(), std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << out.dump(2);
    return file.good();
}

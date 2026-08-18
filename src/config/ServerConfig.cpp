#include "ServerConfig.h"

#include <windows.h>

#include <shlobj.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "../third_party/json.hpp"

#pragma comment(lib, "crypt32.lib")

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

bool ProtectString(const std::string& plaintext, std::vector<BYTE>& outBlob) {
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output{};
    if (!CryptProtectData(
            &input, L"RemoteCodeServer config secret", nullptr, nullptr, nullptr,
            CRYPTPROTECT_LOCAL_MACHINE, &output)) {
        return false;
    }
    outBlob.assign(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool UnprotectString(const std::vector<BYTE>& blob, std::string& outPlaintext) {
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE*>(blob.data());
    input.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(
            &input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &output)) {
        return false;
    }
    outPlaintext.assign(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

std::string Base64Encode(const std::vector<BYTE>& data) {
    DWORD outLen = 0;
    CryptBinaryToStringA(
        data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        nullptr, &outLen);
    if (outLen == 0) {
        return "";
    }
    std::string result(outLen, '\0');
    CryptBinaryToStringA(
        data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        result.data(), &outLen);
    // The sizing call's outLen includes room for a terminating NUL; this
    // second call overwrites outLen with the actual character count
    // written, which does NOT include it — safe to resize to directly.
    result.resize(outLen);
    return result;
}

std::vector<BYTE> Base64Decode(const std::string& text) {
    DWORD outLen = 0;
    CryptStringToBinaryA(
        text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &outLen,
        nullptr, nullptr);
    if (outLen == 0) {
        return {};
    }
    std::vector<BYTE> result(outLen);
    CryptStringToBinaryA(
        text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, result.data(), &outLen,
        nullptr, nullptr);
    return result;
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
        const std::vector<BYTE> blob = Base64Decode(parsed["discord_bot_token_encrypted"].get<std::string>());
        std::string decrypted;
        if (!blob.empty() && UnprotectString(blob, decrypted)) {
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
        std::vector<BYTE> blob;
        if (!ProtectString(config.discordBotToken, blob)) {
            return false;
        }
        out["discord_bot_token_encrypted"] = Base64Encode(blob);
    }

    const std::wstring path = ConfigFilePath();
    std::ofstream file(path.c_str(), std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << out.dump(2);
    return file.good();
}

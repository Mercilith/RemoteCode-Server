#include "ServerIdentity.h"

#include <windows.h>

#include <bcrypt.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "../third_party/monocypher.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace fs = std::filesystem;

namespace {

bool RandomBytes(uint8_t* buffer, size_t size) {
    NTSTATUS status = BCryptGenRandom(
        nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0; // STATUS_SUCCESS
}

std::wstring ProgramDataDir() {
    PWSTR programData = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData);
    if (FAILED(hr) || programData == nullptr) {
        if (programData != nullptr) {
            CoTaskMemFree(programData);
        }
        return L"";
    }
    std::wstring result = std::wstring(programData) + L"\\RemoteCode\\Server";
    CoTaskMemFree(programData);
    return result;
}

bool ProtectSecretKey(const uint8_t secretKey[32], std::vector<BYTE>& outBlob) {
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE*>(secretKey);
    input.cbData = 32;

    DATA_BLOB output{};
    // Machine-scope: decryptable by any process on this machine, not tied
    // to the identity of whichever account happens to run the service.
    if (!CryptProtectData(
            &input, L"RemoteCodeServer identity", nullptr, nullptr, nullptr,
            CRYPTPROTECT_LOCAL_MACHINE, &output)) {
        return false;
    }
    outBlob.assign(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool UnprotectSecretKey(const std::vector<BYTE>& blob, uint8_t secretKey[32]) {
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE*>(blob.data());
    input.cbData = static_cast<DWORD>(blob.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(
            &input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE, &output)) {
        return false;
    }
    bool ok = output.cbData == 32;
    if (ok) {
        memcpy(secretKey, output.pbData, 32);
    }
    crypto_wipe(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return ok;
}

} // namespace

std::wstring ServerIdentityStore::IdentityFilePath() {
    const std::wstring dir = ProgramDataDir();
    if (dir.empty()) {
        return L"";
    }
    return dir + L"\\identity.dat";
}

bool ServerIdentityStore::GenerateNew(ServerIdentity& outIdentity) {
    uint8_t seed[32];
    if (!RandomBytes(seed, sizeof(seed))) {
        return false;
    }
    memcpy(outIdentity.secretKey, seed, 32);
    crypto_x25519_public_key(outIdentity.publicKey, outIdentity.secretKey);
    crypto_wipe(seed, sizeof(seed));
    return true;
}

bool ServerIdentityStore::Save(const ServerIdentity& identity) {
    const std::wstring dir = ProgramDataDir();
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return false;
    }

    std::vector<BYTE> encryptedSecret;
    if (!ProtectSecretKey(identity.secretKey, encryptedSecret)) {
        return false;
    }

    const std::wstring path = IdentityFilePath();
    std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(identity.publicKey), 32);
    file.write(reinterpret_cast<const char*>(encryptedSecret.data()), encryptedSecret.size());
    return file.good();
}

bool ServerIdentityStore::Load(ServerIdentity& outIdentity) {
    const std::wstring path = IdentityFilePath();
    if (path.empty()) {
        return false;
    }
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.read(reinterpret_cast<char*>(outIdentity.publicKey), 32);
    if (!file.good()) {
        return false;
    }

    std::vector<BYTE> encryptedSecret(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (encryptedSecret.empty()) {
        return false;
    }

    return UnprotectSecretKey(encryptedSecret, outIdentity.secretKey);
}

bool ServerIdentityStore::LoadOrCreate(ServerIdentity& outIdentity) {
    if (Load(outIdentity)) {
        return true;
    }

    ServerIdentity fresh;
    if (!GenerateNew(fresh)) {
        return false;
    }
    if (!Save(fresh)) {
        return false;
    }
    outIdentity = fresh;
    return true;
}

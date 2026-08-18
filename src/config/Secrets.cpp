#include "Secrets.h"

#include <windows.h>

#include <wincrypt.h>

#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace {

bool ProtectBytes(const std::string& plaintext, std::vector<BYTE>& outBlob) {
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output{};
    if (!CryptProtectData(
            &input, L"RemoteCodeServer secret", nullptr, nullptr, nullptr, CRYPTPROTECT_LOCAL_MACHINE,
            &output)) {
        return false;
    }
    outBlob.assign(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool UnprotectBytes(const std::vector<BYTE>& blob, std::string& outPlaintext) {
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
        data.data(), static_cast<DWORD>(data.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
        &outLen);
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
        text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &outLen, nullptr,
        nullptr);
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

std::string Secrets::Protect(const std::string& plaintext) {
    std::vector<BYTE> blob;
    if (!ProtectBytes(plaintext, blob)) {
        return "";
    }
    return Base64Encode(blob);
}

std::string Secrets::Unprotect(const std::string& protectedBase64) {
    const std::vector<BYTE> blob = Base64Decode(protectedBase64);
    if (blob.empty()) {
        return "";
    }
    std::string plaintext;
    if (!UnprotectBytes(blob, plaintext)) {
        return "";
    }
    return plaintext;
}

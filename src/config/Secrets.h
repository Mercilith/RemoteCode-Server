#pragma once

#include <string>

// DPAPI machine-scope secret protection + base64 framing, shared by
// anything that needs to persist a secret at rest (the Discord bot token in
// config.json, and now per-agent Discord bot tokens in the agents table).
// CRYPTPROTECT_LOCAL_MACHINE means any process on this machine can decrypt
// it regardless of which user account is running — needed since the
// Windows Service (SYSTEM) and ad-hoc console-mode runs (the logged-in
// user) both need to read the same secrets.
class Secrets {
public:
    // Encrypts `plaintext` and returns it as a base64 string ready to drop
    // straight into a JSON/TEXT field. Empty string on failure.
    static std::string Protect(const std::string& plaintext);

    // Reverses Protect(). Empty string on failure (including "not valid
    // base64" or "not a DPAPI blob this machine can decrypt").
    static std::string Unprotect(const std::string& protectedBase64);
};

#pragma once

#include <cstdint>
#include <string>

// The server's long-term X25519 identity keypair. The public half is what
// a phone app will eventually learn (via a pairing QR code shown by the
// desktop app) to compute a shared secret with; the private half never
// leaves this machine. Also doubles as the input to topic derivation (see
// topic/TopicDerivation.h) — the public key, not the secret one, since the
// topic itself isn't meant to be secret.
struct ServerIdentity {
    uint8_t publicKey[32] = {};
    uint8_t secretKey[32] = {};
};

// Loads the identity from disk, or generates and persists a new one on
// first run. Storage: %ProgramData%\RemoteCode\Server\identity.dat — a
// machine-wide location, since this runs as a Windows Service (typically
// under LocalSystem, which has no meaningful per-user profile) rather than
// a per-user %APPDATA%/%LOCALAPPDATA% location. The secret key is
// encrypted at rest with DPAPI machine-scope protection
// (CRYPTPROTECT_LOCAL_MACHINE) before being written; the public key is
// stored in plaintext alongside it.
class ServerIdentityStore {
public:
    // Returns false only on an unrecoverable I/O, RNG, or DPAPI failure.
    static bool LoadOrCreate(ServerIdentity& outIdentity);

    static std::wstring IdentityFilePath();

private:
    static bool GenerateNew(ServerIdentity& outIdentity);
    static bool Save(const ServerIdentity& identity);
    static bool Load(ServerIdentity& outIdentity);
};

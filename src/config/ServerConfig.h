#pragma once

#include <string>

struct ServerConfig {
    std::string discordBotToken; // plaintext once loaded into memory
    std::string discordGuildId;
    // Cardon's Discord user id — the only human ever granted access to a
    // per-agent private DM channel (see Orchestrator::EnsureChannelForChat).
    // No auto-detection exists for "who is Cardon" and doesn't need to on
    // a personal single-user system; set by hand like discordGuildId.
    std::string discordOwnerUserId;
    // Optional. Points the per-turn worker subprocess's CLAUDE_CONFIG_DIR
    // at an already-`claude /login`'d user's config directory (normally
    // %USERPROFILE%\.claude) — needed because the Windows Service runs as
    // SYSTEM, which has no OAuth session of its own. Empty means "use
    // whatever CLAUDE_CONFIG_DIR/default the worker process would anyway",
    // which only works if you've set ANTHROPIC_API_KEY machine-wide instead.
    std::string claudeConfigDir;
    // False if the config file is missing or the token is empty — this is
    // not treated as an error by the caller (Orchestrator logs a clear
    // message and idles rather than crash-looping).
    bool valid = false;
};

// Loads/saves the local Discord bot config. File:
// %ProgramData%\RemoteCode\ServerData\config.json — a sibling of the
// install directory, same location the removed identity module used to
// live in.
//
// You'll need to create a Discord Application + bot yourself (Discord
// Developer Portal -> New Application -> Bot -> copy the token; invite it
// to your server with Manage Channels, Manage Webhooks, Read Message
// History, Send Messages, Add Reactions, Use Application Commands) and
// hand-write this file with your token + guild id the first time — there's
// no setup UI yet. Format for a fresh file:
//   { "discord_bot_token": "...", "discord_guild_id": "...",
//     "discord_owner_user_id": "...",
//     "claude_config_dir": "C:\\Users\\you\\.claude" }
// claude_config_dir and discord_owner_user_id are optional — see
// ServerConfig::claudeConfigDir/discordOwnerUserId (per-agent DM channels
// are simply never created without the latter set).
// On first load, the plaintext token is immediately replaced on disk with
// a DPAPI machine-scope-encrypted form (field renamed to
// "discord_bot_token_encrypted") — same CryptProtectData pattern the
// removed identity module used — so the plaintext token doesn't sit on
// disk indefinitely.
class ServerConfigStore {
public:
    static std::wstring ConfigFilePath();

    static ServerConfig Load();
    static bool Save(const ServerConfig& config);
};

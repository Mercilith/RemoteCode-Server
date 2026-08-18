#pragma once

#include <string>
#include <vector>

#include "../db/AgentStore.h"

// Parsing and Discord-reflection for the `@agent_id` / `@Agent Name`
// addressing tags agents can put in a message to re-engage another agent
// (see Orchestrator's tag-driven dispatch queue) — kept in one place so the
// matching rule (case-insensitive against id or Slugify(name)) can't drift
// between the dispatch decision and how it's shown on Discord.
namespace Mentions {

// Resolves every `@tag` in `content` against `candidates` (matched
// case-insensitively against each candidate's id, or Slugify(name)).
// Returns the distinct set of matched agent ids, first-seen order. A tag
// that matches no candidate is silently ignored — a wrong guess just
// doesn't trigger anyone, rather than failing the turn.
std::vector<std::string> ParseMentions(const std::string& content, const std::vector<Agent>& candidates);

// Rewrites every resolvable `@tag` in `content` into Discord-ready text: a
// real `<@discordBotUserId>` mention (clickable, pings) if that agent has
// its own bot, otherwise bold plain text (`**@Name**`) since a
// shared-webhook agent has no real Discord account to mention. Tags that
// don't resolve are left exactly as written.
std::string ReflectMentionsForDiscord(const std::string& content, const std::vector<Agent>& candidates);

// True if `sender.canMessageJson` permits addressing `targetAgentId` — the
// array must contain that id, or the wildcard "*". Malformed/missing JSON
// is treated as "no permissions" (fails closed).
bool IsAllowedToMessage(const Agent& sender, const std::string& targetAgentId);

} // namespace Mentions

#pragma once

#include <string>
#include <vector>

// Lowercase, alnum-and-hyphen id derived from a display name (e.g. agent
// names -> agent ids). Used both when Alex proposes a new agent
// (mcp/Tools.cpp) and when the desktop admin API creates one directly
// (http/AdminServer.cpp).
std::string Slugify(const std::string& name);

// Splits `content` into chunks Discord will actually accept as message
// content (a hard 2000-character cap per message — found the hard way: an
// agent's longer report silently never appeared in Discord, stored in the
// DB but never posted, no error surfaced anywhere). Prefers to break on a
// newline near the limit so paragraphs/lists don't get cut mid-line where
// avoidable; falls back to a hard split if no good newline is nearby.
// Returns a single (possibly empty) chunk if content already fits.
std::vector<std::string> ChunkForDiscord(const std::string& content);

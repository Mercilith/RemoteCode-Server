#pragma once

#include <string>

// Lowercase, alnum-and-hyphen id derived from a display name (e.g. agent
// names -> agent ids). Used both when Alex proposes a new agent
// (mcp/Tools.cpp) and when the desktop admin API creates one directly
// (http/AdminServer.cpp).
std::string Slugify(const std::string& name);

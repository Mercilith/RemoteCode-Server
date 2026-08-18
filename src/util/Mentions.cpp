#include "Mentions.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "../third_party/json.hpp"
#include "Text.h"

namespace {

std::string ToLower(const std::string& s) {
    std::string result = s;
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

const std::regex& TagPattern() {
    static const std::regex pattern("@([A-Za-z0-9_-]+)");
    return pattern;
}

const Agent* FindByTag(const std::string& tag, const std::vector<Agent>& candidates) {
    const std::string lowerTag = ToLower(tag);
    for (const Agent& a : candidates) {
        if (ToLower(a.id) == lowerTag) {
            return &a;
        }
    }
    for (const Agent& a : candidates) {
        if (ToLower(Slugify(a.name)) == lowerTag) {
            return &a;
        }
    }
    return nullptr;
}

} // namespace

namespace Mentions {

std::vector<std::string> ParseMentions(const std::string& content, const std::vector<Agent>& candidates) {
    std::vector<std::string> result;
    for (auto it = std::sregex_iterator(content.begin(), content.end(), TagPattern()); it != std::sregex_iterator();
         ++it) {
        const Agent* match = FindByTag((*it)[1].str(), candidates);
        if (match == nullptr) {
            continue;
        }
        if (std::find(result.begin(), result.end(), match->id) == result.end()) {
            result.push_back(match->id);
        }
    }
    return result;
}

std::string ReflectMentionsForDiscord(const std::string& content, const std::vector<Agent>& candidates) {
    std::string result;
    result.reserve(content.size());
    size_t lastEnd = 0;
    for (auto it = std::sregex_iterator(content.begin(), content.end(), TagPattern()); it != std::sregex_iterator();
         ++it) {
        const std::smatch& m = *it;
        const size_t matchStart = static_cast<size_t>(m.position(0));
        const size_t matchLen = static_cast<size_t>(m.length(0));
        result.append(content, lastEnd, matchStart - lastEnd);

        const Agent* match = FindByTag(m[1].str(), candidates);
        if (match == nullptr) {
            result.append(m.str(0));
        } else if (!match->discordBotUserId.empty()) {
            result += "<@" + match->discordBotUserId + ">";
        } else {
            result += "**@" + match->name + "**";
        }
        lastEnd = matchStart + matchLen;
    }
    result.append(content, lastEnd, content.size() - lastEnd);
    return result;
}

bool IsAllowedToMessage(const Agent& sender, const std::string& targetAgentId) {
    nlohmann::json canMessage;
    try {
        canMessage = nlohmann::json::parse(sender.canMessageJson);
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
    if (!canMessage.is_array()) {
        return false;
    }
    for (const nlohmann::json& entry : canMessage) {
        if (!entry.is_string()) {
            continue;
        }
        const std::string value = entry.get<std::string>();
        if (value == "*" || value == targetAgentId) {
            return true;
        }
    }
    return false;
}

} // namespace Mentions

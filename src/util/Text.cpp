#include "Text.h"

#include <cctype>

std::vector<std::string> ChunkForDiscord(const std::string& content) {
    constexpr size_t kMaxLen = 1990; // margin below Discord's 2000-char hard cap
    std::vector<std::string> chunks;
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t remaining = content.size() - pos;
        if (remaining <= kMaxLen) {
            chunks.push_back(content.substr(pos));
            break;
        }
        size_t splitAt = content.rfind('\n', pos + kMaxLen);
        if (splitAt == std::string::npos || splitAt <= pos) {
            splitAt = pos + kMaxLen; // no good newline nearby — hard split
        }
        chunks.push_back(content.substr(pos, splitAt - pos));
        pos = splitAt;
        while (pos < content.size() && content[pos] == '\n') {
            ++pos; // don't carry the split newline itself into the next chunk
        }
    }
    if (chunks.empty()) {
        chunks.push_back(content); // content was empty or fully whitespace-trimmed away
    }
    return chunks;
}

std::string Slugify(const std::string& name) {
    std::string slug;
    slug.reserve(name.size());
    bool lastWasDash = false;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            lastWasDash = false;
        } else if (!lastWasDash && !slug.empty()) {
            slug.push_back('-');
            lastWasDash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug.empty() ? "agent" : slug;
}

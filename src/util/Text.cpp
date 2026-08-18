#include "Text.h"

#include <cctype>

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

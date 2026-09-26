#pragma once
#include <string>
#include <unordered_set>

namespace dice {
// The management library must show saved but empty/shadowed documents, while
// ordinary help queries keep their existing first-nonempty-topic behavior.
class HelpEntryFilter {
public:
    struct Decision { bool include; bool shadowed; };
    explicit HelpEntryFilter(bool management) : management_(management) {}
    Decision add(const std::string& key, const std::string& content) {
        if (key.empty()) return {false, false};
        const bool shadowed = !content.empty() && !seen_.insert(key).second;
        return {management_ || (!content.empty() && !shadowed), shadowed};
    }
private:
    bool management_;
    std::unordered_set<std::string> seen_;
};
}

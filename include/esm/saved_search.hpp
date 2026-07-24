#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace esm {

struct SavedSearch {
    std::wstring name;
    std::wstring query;
    bool case_sensitive{};
    bool whole_word{};
    bool match_path{};
    bool match_diacritics{};
    bool regex{};
};

[[nodiscard]] std::string serialize_saved_searches(
    const std::vector<SavedSearch>& values);
[[nodiscard]] bool parse_saved_searches(std::string_view text,
                                        std::vector<SavedSearch>& values);

} // namespace esm

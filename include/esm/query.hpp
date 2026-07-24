#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace esm {
enum class MatchTarget {
    any, name, path, extension, size, last_write_time, attributes
};
enum class NumericComparison { equal, less, less_equal, greater, greater_equal };
enum class QueryOpcode { term, logical_and, logical_or, logical_not };
enum class DuplicateMode { none, name, size, name_and_size };
struct QueryTerm {
    MatchTarget target{MatchTarget::any};
    std::wstring value;
    bool excluded{};
    bool wildcard{};
    bool regex{};
    NumericComparison comparison{NumericComparison::equal};
    std::uint64_t numeric_value{};
    std::uint32_t attribute_mask{};
    bool attribute_absent{};
};
struct QueryInstruction {
    QueryOpcode opcode{QueryOpcode::term};
    std::size_t term_index{};
};
struct ParsedQuery {
    std::vector<QueryTerm> terms;
    std::vector<QueryInstruction> program;
    std::optional<bool> directories_only;
    DuplicateMode duplicate_mode{DuplicateMode::none};
    std::optional<bool> case_sensitive;
    std::optional<bool> whole_word;
    bool valid{true};
    std::wstring error;
};
[[nodiscard]] ParsedQuery parse_query(std::wstring_view text);
[[nodiscard]] std::wstring fold_case(std::wstring_view text);
[[nodiscard]] bool wildcard_match(std::wstring_view pattern, std::wstring_view value);
// Explorer-style natural comparison: digit runs compare numerically and text
// compares case-insensitively unless requested otherwise.
[[nodiscard]] int natural_compare(std::wstring_view left,
                                  std::wstring_view right,
                                  bool case_sensitive = false);
}

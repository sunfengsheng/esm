#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace esm {
enum class MatchTarget {
    any,
    name,
    path,
    filename_list,
    extension,
    name_prefix,
    name_suffix,
    filename_length,
    path_depth,
    parent_path,
    child_name,
    direct_child_count,
    child_file_count,
    child_folder_count,
    size,
    last_write_time,
    attributes
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
    bool has_lower_bound{};
    bool lower_inclusive{true};
    std::uint64_t lower_bound{};
    bool has_upper_bound{};
    bool upper_inclusive{true};
    std::uint64_t upper_bound{};
    std::vector<std::wstring> alternatives;
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
    std::optional<std::size_t> max_results;
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

#include "esm/index.hpp"
#include "esm/file_metadata.hpp"
#include "esm/volume_discovery.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cwctype>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <regex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <windows.h>
#include <winioctl.h>
namespace esm {
namespace {
wchar_t normalize_char(wchar_t ch,bool sensitive){if(sensitive)return ch;if(ch>=L'A'&&ch<=L'Z')return (wchar_t)(ch+(L'a'-L'A'));if(ch<128)return ch;return (wchar_t)std::towlower(ch);}
void hash_name_character(std::uint32_t& hash, wchar_t ch) noexcept {
    const auto folded = static_cast<std::uint16_t>(normalize_char(ch, false));
    hash ^= folded & 0xffU;
    hash *= 16777619U;
    hash ^= (folded >> 8U) & 0xffU;
    hash *= 16777619U;
}
std::uint32_t name_bigram_key(wchar_t a, wchar_t b) noexcept {
    std::uint32_t hash = 2166136261U;
    hash_name_character(hash, a);
    hash_name_character(hash, b);
    return hash;
}
std::uint32_t name_trigram_key(wchar_t a, wchar_t b, wchar_t c) noexcept {
    std::uint32_t hash = 2166136261U;
    hash_name_character(hash, a);
    hash_name_character(hash, b);
    hash_name_character(hash, c);
    return hash;
}
constexpr std::size_t name_trigram_bucket_count = std::size_t{1} << 16U;
constexpr std::uint32_t name_trigram_bucket_mask = 0xffffU;
std::uint32_t name_prefix_key(std::wstring_view value) noexcept {
    if (value.empty()) return 0;
    const auto first = static_cast<std::uint16_t>(normalize_char(value[0], false));
    const auto second = value.size() > 1
        ? static_cast<std::uint16_t>(normalize_char(value[1], false))
        : std::uint16_t{};
    return (static_cast<std::uint32_t>(first) << 16U) | second;
}
constexpr std::uint64_t unsafe_natural_prefix = std::uint64_t{1} << 63U;
std::uint64_t natural_name_sort_prefix(std::wstring_view value) noexcept {
    // Three case-folded natural-order tokens distinguish the overwhelming
    // majority of comparisons without touching either string again. ASCII
    // digit runs deliberately share one token because natural_compare orders
    // the complete run numerically. Rare non-ASCII digit prefixes fall back
    // to the full comparator to preserve exact Unicode behavior.
    std::uint64_t key = 0;
    bool stopped = false;
    bool safe = true;
    for (std::size_t offset = 0; offset < 3; ++offset) {
        std::uint32_t token = 0;
        if (!stopped && offset < value.size()) {
            const auto ch = value[offset];
            if (std::iswdigit(ch)) {
                if (ch < L'0' || ch > L'9') safe = false;
                token = static_cast<std::uint32_t>(L'0') + 1U;
                stopped = true;
            } else {
                token = static_cast<std::uint16_t>(
                            normalize_char(ch, false)) + 1U;
            }
        }
        key = (key << 17U) | token;
    }
    return safe ? key : key | unsafe_natural_prefix;
}
std::vector<std::size_t> mandatory_term_indices(const ParsedQuery& query){
    std::vector<std::vector<std::size_t>> stack;
    stack.reserve(query.program.size());
    for(const auto& instruction:query.program){
        if(instruction.opcode==QueryOpcode::term){
            stack.push_back({instruction.term_index});
            continue;
        }
        if(stack.empty())return {};
        if(instruction.opcode==QueryOpcode::logical_not){
            stack.back().clear();
            continue;
        }
        if(stack.size()<2)return {};
        auto right=std::move(stack.back());stack.pop_back();
        auto left=std::move(stack.back());stack.pop_back();
        std::sort(left.begin(),left.end());
        left.erase(std::unique(left.begin(),left.end()),left.end());
        std::sort(right.begin(),right.end());
        right.erase(std::unique(right.begin(),right.end()),right.end());
        std::vector<std::size_t> combined;
        if(instruction.opcode==QueryOpcode::logical_and){
            combined.reserve(left.size()+right.size());
            std::set_union(left.begin(),left.end(),right.begin(),right.end(),
                           std::back_inserter(combined));
        }else{
            combined.reserve(std::min(left.size(),right.size()));
            std::set_intersection(left.begin(),left.end(),right.begin(),right.end(),
                                  std::back_inserter(combined));
        }
        stack.push_back(std::move(combined));
    }
    return stack.size()==1?std::move(stack.back()):std::vector<std::size_t>{};
}
bool word_char(wchar_t ch){return std::iswalnum(ch)||ch==L'_';}
std::wstring normalize_match_text(std::wstring_view value, bool sensitive,
                                  bool match_diacritics) {
    std::wstring folded = sensitive ? std::wstring(value) : fold_case(value);
    if (match_diacritics || folded.empty()) return folded;
    const int required = FoldStringW(MAP_COMPOSITE, folded.data(),
                                     static_cast<int>(folded.size()), nullptr, 0);
    if (required <= 0) return folded;
    std::wstring decomposed(static_cast<std::size_t>(required), L'\0');
    const int written = FoldStringW(MAP_COMPOSITE, folded.data(),
                                    static_cast<int>(folded.size()),
                                    decomposed.data(), required);
    if (written <= 0) return folded;
    decomposed.resize(static_cast<std::size_t>(written));
    std::vector<WORD> types(decomposed.size());
    if (!GetStringTypeW(CT_CTYPE3, decomposed.data(),
                        static_cast<int>(decomposed.size()), types.data()))
        return folded;
    std::wstring result;
    result.reserve(decomposed.size());
    for (std::size_t i = 0; i < decomposed.size(); ++i) {
        if ((types[i] & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) == 0)
            result.push_back(decomposed[i]);
    }
    return result;
}
struct CompiledTerm {
    MatchTarget target{MatchTarget::any};
    std::wstring value;
    std::wstring wildcard_pattern;
    std::vector<std::wstring> alternatives;
    bool excluded{};
    bool wildcard{};
    bool regex{};
    bool valid{true};
    bool match_diacritics{true};
    NumericComparison comparison{NumericComparison::equal};
    std::uint64_t numeric_value{};
    bool has_lower_bound{};
    bool lower_inclusive{true};
    std::uint64_t lower_bound{};
    bool has_upper_bound{};
    bool upper_inclusive{true};
    std::uint64_t upper_bound{};
    std::uint32_t attribute_mask{};
    bool attribute_absent{};
    std::optional<std::wregex> regex_pattern;
    std::array<std::uint32_t, 256> skip{};
};
CompiledTerm compile_term(const QueryTerm& term, bool sensitive,
                          bool match_diacritics) {
    CompiledTerm result;
    result.target = term.target;
    result.match_diacritics = match_diacritics;
    result.value = normalize_match_text(term.value, sensitive,
                                        match_diacritics);
    result.alternatives.reserve(term.alternatives.size());
    for (const auto& alternative : term.alternatives) {
        auto normalized = normalize_match_text(
            alternative, sensitive, match_diacritics);
        if (term.target == MatchTarget::filename_list) {
            std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        }
        result.alternatives.push_back(std::move(normalized));
    }
    result.excluded = term.excluded;
    result.wildcard = term.wildcard;
    result.regex = term.regex;
    result.comparison = term.comparison;
    result.numeric_value = term.numeric_value;
    result.has_lower_bound = term.has_lower_bound;
    result.lower_inclusive = term.lower_inclusive;
    result.lower_bound = term.lower_bound;
    result.has_upper_bound = term.has_upper_bound;
    result.upper_inclusive = term.upper_inclusive;
    result.upper_bound = term.upper_bound;
    result.attribute_mask = term.attribute_mask;
    result.attribute_absent = term.attribute_absent;
    result.skip.fill(static_cast<std::uint32_t>(
        std::max<std::size_t>(1, result.value.size())));
    if (result.value.size() > 1) {
        for (std::size_t index = 0; index + 1 < result.value.size(); ++index) {
            if (result.value[index] < 256) {
                result.skip[static_cast<unsigned>(result.value[index])] =
                    static_cast<std::uint32_t>(result.value.size() - index - 1);
            }
        }
    }
    if (result.wildcard) result.wildcard_pattern = L"*" + result.value + L"*";
    if (result.regex) {
        try {
            const auto flags = std::regex_constants::ECMAScript;
            const auto pattern = normalize_match_text(term.value, sensitive,
                                                      match_diacritics);
            result.regex_pattern.emplace(pattern, flags);
        } catch (const std::regex_error&) {
            result.valid = false;
        }
    }
    return result;
}
bool starts_text(std::wstring_view text,const CompiledTerm& term,bool sensitive){if(text.size()<term.value.size())return false;for(std::size_t i=0;i<term.value.size();++i)if(normalize_char(text[i],sensitive)!=term.value[i])return false;return true;}
bool equal_text(std::wstring_view text,const CompiledTerm& term,bool sensitive){return text.size()==term.value.size()&&starts_text(text,term,sensitive);}
bool contains_text(std::wstring_view text,const CompiledTerm& term,bool sensitive,bool whole_word){auto needle=std::wstring_view(term.value);if(needle.empty())return true;if(needle.size()>text.size())return false;std::size_t offset=0;while(offset+needle.size()<=text.size()){std::size_t j=needle.size();while(j&&normalize_char(text[offset+j-1],sensitive)==needle[j-1])--j;if(!j){bool left=offset==0||!word_char(text[offset-1]);bool right=offset+needle.size()==text.size()||!word_char(text[offset+needle.size()]);if(!whole_word||(left&&right))return true;++offset;continue;}wchar_t tail=normalize_char(text[offset+needle.size()-1],sensitive);std::size_t shift=needle.size();if(tail<256)shift=term.skip[(unsigned)tail];else for(std::size_t i=0;i+1<needle.size();++i)if(needle[i]==tail)shift=needle.size()-i-1;offset+=std::max<std::size_t>(1,shift);}return false;}
bool wildcard_text(const CompiledTerm& term,std::wstring_view value,bool sensitive){auto ptn=std::wstring_view(term.wildcard_pattern);std::size_t p=0,v=0,star=std::wstring_view::npos,checkpoint=0;while(v<value.size()){bool same=p<ptn.size()&&ptn[p]!=L'*'&&ptn[p]!=L'?'&&normalize_char(value[v],sensitive)==ptn[p];if(p<ptn.size()&&(ptn[p]==L'?'||same)){++p;++v;}else if(p<ptn.size()&&ptn[p]==L'*'){star=p++;checkpoint=v;}else if(star!=std::wstring_view::npos){p=star+1;v=++checkpoint;}else return false;}while(p<ptn.size()&&ptn[p]==L'*')++p;return p==ptn.size();}
std::wstring_view extension_of(std::wstring_view name){auto dot=name.find_last_of(L'.');return dot==std::wstring_view::npos||dot+1==name.size()?std::wstring_view{}:name.substr(dot+1);}
bool compare_number(std::uint64_t value, const CompiledTerm& term) {
    if (term.has_lower_bound) {
        if (value < term.lower_bound ||
            (!term.lower_inclusive && value == term.lower_bound)) {
            return false;
        }
    }
    if (term.has_upper_bound) {
        if (value > term.upper_bound ||
            (!term.upper_inclusive && value == term.upper_bound)) {
            return false;
        }
    }
    if (term.has_lower_bound || term.has_upper_bound) return true;
    switch (term.comparison) {
    case NumericComparison::equal: return value == term.numeric_value;
    case NumericComparison::less: return value < term.numeric_value;
    case NumericComparison::less_equal: return value <= term.numeric_value;
    case NumericComparison::greater: return value > term.numeric_value;
    case NumericComparison::greater_equal: return value >= term.numeric_value;
    }
    return false;
}
bool has_non_ascii(std::wstring_view value) {
    return std::any_of(value.begin(), value.end(),
                       [](wchar_t ch) { return ch >= 0x80; });
}
bool text_match(const CompiledTerm& term, std::wstring_view value,
                bool sensitive, bool whole_word) {
    if (!term.match_diacritics) {
        // Almost all catalog names are ASCII. Try the allocation-free matcher
        // first and only invoke Unicode decomposition for a non-ASCII value
        // that did not already match. This keeps Everything's default
        // ignore-diacritics mode on the same hot path as ordinary matching.
        if (!term.regex) {
            const bool raw_match = term.wildcard
                ? wildcard_text(term, value, sensitive)
                : contains_text(value, term, sensitive, whole_word);
            const bool non_ascii = has_non_ascii(value);
            // Combining marks can change whole-word boundaries after folding,
            // so verify those uncommon non-ASCII successes on normalized text.
            if (raw_match && (!whole_word || !non_ascii)) return true;
            if (!non_ascii) return false;
        }
        const auto normalized = normalize_match_text(value, sensitive, false);
        if (term.regex) {
            return term.regex_pattern &&
                std::regex_search(normalized.begin(), normalized.end(),
                                  *term.regex_pattern);
        }
        return term.wildcard ? wildcard_text(term, normalized, true)
                             : contains_text(normalized, term, true, whole_word);
    }
    if (term.regex) {
        return term.regex_pattern &&
            std::regex_search(value.begin(), value.end(), *term.regex_pattern);
    }
    return term.wildcard ? wildcard_text(term, value, sensitive)
                         : contains_text(value, term, sensitive, whole_word);
}
bool exact_text_match(const CompiledTerm& term, std::wstring_view value,
                      bool sensitive) {
    if (!term.match_diacritics) {
        if (equal_text(value, term, sensitive)) return true;
        if (!has_non_ascii(value)) return false;
        const auto normalized = normalize_match_text(value, sensitive, false);
        return equal_text(normalized, term, true);
    }
    return equal_text(value, term, sensitive);
}
bool prefix_text_match(const CompiledTerm& term, std::wstring_view value,
                       bool sensitive) {
    if (!term.match_diacritics) {
        if (starts_text(value, term, sensitive)) return true;
        if (!has_non_ascii(value)) return false;
        const auto normalized = normalize_match_text(value, sensitive, false);
        return starts_text(normalized, term, true);
    }
    return starts_text(value, term, sensitive);
}
bool suffix_text_match(const CompiledTerm& term, std::wstring_view value,
                       bool sensitive) {
    const auto ends_with_term = [&](std::wstring_view candidate,
                                    bool normalized) {
        if (candidate.size() < term.value.size()) return false;
        const auto suffix = candidate.substr(candidate.size() - term.value.size());
        return equal_text(suffix, term, normalized ? true : sensitive);
    };
    if (!term.match_diacritics) {
        if (ends_with_term(value, false)) return true;
        if (!has_non_ascii(value)) return false;
        const auto normalized = normalize_match_text(value, sensitive, false);
        return ends_with_term(normalized, true);
    }
    return ends_with_term(value, false);
}
bool anchored_wildcard_match(std::wstring_view pattern,
                             std::wstring_view value, bool sensitive) {
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star = std::wstring_view::npos;
    std::size_t checkpoint = 0;
    while (value_index < value.size()) {
        const bool same = pattern_index < pattern.size() &&
            pattern[pattern_index] != L'*' && pattern[pattern_index] != L'?' &&
            normalize_char(value[value_index], sensitive) == pattern[pattern_index];
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == L'?' || same)) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < pattern.size() &&
                   pattern[pattern_index] == L'*') {
            star = pattern_index++;
            checkpoint = value_index;
        } else if (star != std::wstring_view::npos) {
            pattern_index = star + 1;
            value_index = ++checkpoint;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == L'*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

bool filename_list_match(const CompiledTerm& term, std::wstring_view name,
                         std::wstring_view path, bool sensitive) {
    const auto matches = [&](std::wstring_view candidate,
                             std::wstring_view pattern, bool normalized) {
        if (pattern.find_first_of(L"*?") != std::wstring::npos) {
            if (candidate.find(L'/') != std::wstring_view::npos) {
                auto normalized_separators = std::wstring(candidate);
                std::replace(normalized_separators.begin(),
                             normalized_separators.end(), L'/', L'\\');
                return anchored_wildcard_match(
                    pattern, normalized_separators,
                    normalized ? true : sensitive);
            }
            return anchored_wildcard_match(pattern, candidate,
                                           normalized ? true : sensitive);
        }
        if (pattern.size() != candidate.size()) return false;
        for (std::size_t index = 0; index < pattern.size(); ++index) {
            wchar_t candidate_char = candidate[index];
            if (candidate_char == L'/') candidate_char = L'\\';
            if (normalize_char(candidate_char, normalized ? true : sensitive) !=
                pattern[index]) {
                return false;
            }
        }
        return true;
    };

    for (const auto& pattern : term.alternatives) {
        const bool path_pattern =
            pattern.find_first_of(L"\\:") != std::wstring::npos;
        const auto candidate = path_pattern ? path : name;
        if (matches(candidate, pattern, false)) return true;
        if (!term.match_diacritics && has_non_ascii(candidate)) {
            auto normalized = normalize_match_text(candidate, sensitive, false);
            if (path_pattern) {
                std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
            }
            if (matches(normalized, pattern, true)) return true;
        }
    }
    return false;
}

bool extension_list_match(const CompiledTerm& term, std::wstring_view extension,
                          bool sensitive) {
    const auto matches = [&](std::wstring_view candidate,
                             bool normalized_candidate) {
        for (const auto& pattern : term.alternatives) {
            if (pattern.find_first_of(L"*?") != std::wstring::npos) {
                if (anchored_wildcard_match(pattern, candidate,
                                            normalized_candidate ? true
                                                                 : sensitive)) {
                    return true;
                }
            } else if (pattern.size() == candidate.size()) {
                bool equal = true;
                for (std::size_t index = 0; index < pattern.size(); ++index) {
                    if (normalize_char(candidate[index],
                                       normalized_candidate ? true : sensitive) !=
                        pattern[index]) {
                        equal = false;
                        break;
                    }
                }
                if (equal) return true;
            }
        }
        return false;
    };
    if (matches(extension, false)) return true;
    if (term.match_diacritics || !has_non_ascii(extension)) return false;
    const auto normalized = normalize_match_text(extension, sensitive, false);
    return matches(normalized, true);
}

std::wstring_view parent_path_of(std::wstring_view path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.remove_suffix(1);
    }
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring_view::npos) return {};
    if (separator == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, separator);
}
std::uint64_t path_depth_of(std::wstring_view path) {
    const auto parent = parent_path_of(path);
    if (parent.empty()) return 0;
    std::size_t start = 0;
    if (parent.size() >= 3 && parent[1] == L':' &&
        (parent[2] == L'\\' || parent[2] == L'/')) {
        start = 3;
    } else if (parent.size() >= 2 &&
               (parent[0] == L'\\' || parent[0] == L'/') &&
               (parent[1] == L'\\' || parent[1] == L'/')) {
        const auto server_end = parent.find_first_of(L"\\/", 2);
        if (server_end == std::wstring_view::npos) return 0;
        const auto share_end = parent.find_first_of(L"\\/", server_end + 1);
        start = share_end == std::wstring_view::npos ? parent.size() : share_end + 1;
    }
    std::uint64_t depth = 0;
    bool in_component = false;
    for (std::size_t index = start; index < parent.size(); ++index) {
        const bool separator = parent[index] == L'\\' || parent[index] == L'/';
        if (separator) {
            if (in_component) ++depth;
            in_component = false;
        } else {
            in_component = true;
        }
    }
    if (in_component) ++depth;
    return depth;
}
bool term_matches(const CompiledTerm& term, std::wstring_view name,
                  std::wstring_view path, std::uint64_t size,
                  std::int64_t modified, std::uint32_t attributes,
                  bool directory, bool child_name_match,
                  std::uint64_t child_file_count,
                  std::uint64_t child_folder_count, bool match_path,
                  bool sensitive, bool whole_word) {
    switch (term.target) {
    case MatchTarget::name:
        return text_match(term, name, sensitive, whole_word);
    case MatchTarget::path:
        return text_match(term, path, sensitive, whole_word);
    case MatchTarget::filename_list:
        return filename_list_match(term, name, path, sensitive);
    case MatchTarget::extension: {
        const auto ext = extension_of(name);
        return extension_list_match(term, ext, sensitive);
    }
    case MatchTarget::name_prefix:
        return prefix_text_match(term, name, sensitive);
    case MatchTarget::name_suffix:
        return suffix_text_match(term, name, sensitive);
    case MatchTarget::filename_length:
        return compare_number(name.size(), term);
    case MatchTarget::path_depth:
        return compare_number(path_depth_of(path), term);
    case MatchTarget::parent_path:
        return exact_text_match(term, parent_path_of(path), sensitive);
    case MatchTarget::child_name:
        return directory && child_name_match;
    case MatchTarget::direct_child_count:
        return directory &&
            compare_number(child_file_count + child_folder_count, term);
    case MatchTarget::child_file_count:
        return directory && compare_number(child_file_count, term);
    case MatchTarget::child_folder_count:
        return directory && compare_number(child_folder_count, term);
    case MatchTarget::size:
        return compare_number(size, term);
    case MatchTarget::last_write_time:
        return compare_number(modified < 0 ? 0 :
                              static_cast<std::uint64_t>(modified), term);
    case MatchTarget::attributes: {
        const bool present =
            (attributes & term.attribute_mask) == term.attribute_mask;
        return term.attribute_absent ? !present : present;
    }
    case MatchTarget::any:
        return text_match(term, name, sensitive, whole_word) ||
            (match_path && text_match(term, path, sensitive, whole_word));
    }
    return false;
}
template <typename ChildNameMatch>
bool evaluate(const ParsedQuery& query,
              const std::vector<CompiledTerm>& terms,
              std::wstring_view name, std::wstring_view path,
              std::uint64_t size, std::int64_t modified,
              std::uint32_t attributes, bool directory,
              std::uint64_t child_file_count,
              std::uint64_t child_folder_count,
              ChildNameMatch&& child_name_matches, bool match_path,
              bool sensitive, bool whole_word) {
    if (query.program.empty()) return true;
    std::vector<bool> stack;
    stack.reserve(query.terms.size());
    for (const auto& instruction : query.program) {
        if (instruction.opcode == QueryOpcode::term) {
            if (instruction.term_index >= terms.size()) return false;
            stack.push_back(term_matches(
                terms[instruction.term_index], name, path, size, modified,
                attributes, directory,
                child_name_matches(instruction.term_index), child_file_count,
                child_folder_count, match_path, sensitive, whole_word));
        } else if (instruction.opcode == QueryOpcode::logical_not) {
            if (stack.empty()) return false;
            stack.back() = !stack.back();
        } else {
            if (stack.size() < 2) return false;
            const bool right = stack.back();
            stack.pop_back();
            const bool left = stack.back();
            stack.back() = instruction.opcode == QueryOpcode::logical_and
                ? left && right : left || right;
        }
    }
    return stack.size() == 1 && stack.back();
}
int rank_record(const std::vector<CompiledTerm>& terms,
                std::wstring_view name, std::wstring_view path,
                bool match_path, bool sensitive) {
    int score = 0;
    for (const auto& term : terms) {
        if (term.excluded || term.target == MatchTarget::filename_length ||
            term.target == MatchTarget::path_depth ||
            term.target == MatchTarget::parent_path ||
            term.target == MatchTarget::direct_child_count ||
            term.target == MatchTarget::child_file_count ||
            term.target == MatchTarget::child_folder_count ||
            term.target == MatchTarget::size ||
            term.target == MatchTarget::last_write_time ||
            term.target == MatchTarget::attributes) {
            continue;
        }
        if (term.regex || term.wildcard) {
            score += 20;
            continue;
        }
        if (exact_text_match(term, name, sensitive)) score += 120;
        else if (prefix_text_match(term, name, sensitive)) score += 80;
        else if (text_match(term, name, sensitive, false)) score += 50;
        else if ((term.target == MatchTarget::path || match_path) &&
                 text_match(term, path, sensitive, false)) score += 20;
    }
    return score;
}
struct Candidate {int score{};std::uint64_t id{};std::size_t index{};bool overlay{};};
struct MinScoreFirst {bool operator()(const Candidate& a,const Candidate& b)const{return a.score!=b.score?a.score>b.score:a.id<b.id;}};
} // namespace
MetadataIndex::NameSearchAccelerators
MetadataIndex::build_name_search_accelerators(
    const std::vector<CompactRecord>& records,
    const std::vector<wchar_t>& strings) {
    if (records.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("name accelerator exceeds 32-bit indices");
    }

    const auto record_path = [&](std::size_t index,
                                 std::wstring& scratch) -> std::wstring_view {
        const auto stored_path = [&](const CompactRecord& item) {
            return std::wstring_view(strings.data() + item.path_offset,
                                     item.path_length);
        };
        const auto& record = records[index];
        if (!record.name_only_path) return stored_path(record);

        constexpr std::size_t maximum_components = 512;
        std::array<std::size_t, maximum_components> components{};
        std::size_t component_count = 0;
        std::size_t current_index = index;
        while (records[current_index].name_only_path &&
               component_count < components.size()) {
            components[component_count++] = current_index;
            const auto parent_index = records[current_index].parent_index;
            if (parent_index == missing_parent_index ||
                parent_index >= records.size() ||
                parent_index == current_index) {
                return stored_path(record);
            }
            current_index = parent_index;
        }
        if (records[current_index].name_only_path) return stored_path(record);

        scratch.assign(stored_path(records[current_index]));
        while (component_count != 0) {
            const auto& component = records[components[--component_count]];
            if (!scratch.ends_with(L"\\") && !scratch.ends_with(L"/")) {
                scratch.push_back(L'\\');
            }
            scratch.append(strings.data() + component.name_offset(),
                           component.name_length());
        }
        return scratch;
    };

    NameSearchAccelerators accelerators;
    accelerators.bigram_signatures.resize(records.size());
    std::vector<std::uint64_t> prefix_entries;
    prefix_entries.reserve(records.size());
    std::vector<std::uint64_t> folded_prefix_entries;

    const auto add_gram = [](NameBigramSignature& signature,
                             std::uint32_t hash) {
        for (unsigned shift : {0U, 8U, 16U, 24U}) {
            const auto bit = (hash >> shift) & 0x7fU;
            signature.words[bit >> 6U] |=
                std::uint64_t{1} << (bit & 63U);
        }
    };

    for (std::size_t record_index = 0; record_index < records.size();
         ++record_index) {
        const auto& record = records[record_index];
        const std::wstring_view name(strings.data() + record.name_offset(),
                                     record.name_length());
        if (!name.empty()) {
            const auto key = name_prefix_key(name);
            prefix_entries.push_back(
                (static_cast<std::uint64_t>(key) << 32U) |
                static_cast<std::uint32_t>(record_index));
        }
        const auto add_name_grams = [&](std::wstring_view value) {
            for (std::size_t offset = 0; offset + 1 < value.size(); ++offset) {
                add_gram(accelerators.bigram_signatures[record_index],
                         name_bigram_key(value[offset], value[offset + 1]));
            }
        };
        add_name_grams(name);

        // Store accent-folded grams in the same name Bloom signature. The raw
        // grams remain present, so diacritic-sensitive searches retain their
        // exact semantics.
        if (std::any_of(name.begin(), name.end(),
                        [](wchar_t ch) { return ch >= 0x80; })) {
            const auto folded = normalize_match_text(name, false, false);
            bool differs = folded.size() != name.size();
            if (!differs) {
                for (std::size_t offset = 0; offset < name.size(); ++offset) {
                    if (folded[offset] != normalize_char(name[offset], false)) {
                        differs = true;
                        break;
                    }
                }
            }
            if (differs) {
                add_name_grams(folded);
                if (!folded.empty()) {
                    const auto key = name_prefix_key(folded);
                    folded_prefix_entries.push_back(
                        (static_cast<std::uint64_t>(key) << 32U) |
                        static_cast<std::uint32_t>(record_index));
                }
            }
        }
    }

    // A path term without a separator must be contained either in the
    // record name or in its parent directory path. Keep one 256-bit signature
    // per directory. A compact directory bitset plus prefix ranks derives the
    // signature index for directories and ordinary children; only unusual
    // full-path file anchors need a sparse explicit owner.
    constexpr auto invalid_signature_owner =
        std::numeric_limits<std::uint32_t>::max();
    const auto directory_count = static_cast<std::size_t>(std::count_if(
        records.begin(), records.end(),
        [](const CompactRecord& record) { return record.directory(); }));
    accelerators.directory_signature_bits.assign(
        (records.size() + 63U) / 64U, 0);
    accelerators.path_trigram_signatures.reserve(directory_count);

    const auto make_path_signature = [&](std::wstring_view path) {
        PathTrigramSignature signature{};
        const auto add_path_trigrams = [&](std::wstring_view value) {
            for (std::size_t offset = 0; offset + 2 < value.size(); ++offset) {
                const auto bit = name_trigram_key(
                    value[offset], value[offset + 1], value[offset + 2]) &
                    0xffU;
                signature.words[bit >> 6U] |=
                    std::uint64_t{1} << (bit & 63U);
            }
        };
        add_path_trigrams(path);
        if (std::any_of(path.begin(), path.end(),
                        [](wchar_t ch) { return ch >= 0x80; })) {
            const auto folded = normalize_match_text(path, false, false);
            add_path_trigrams(folded);
        }
        return signature;
    };

    for (std::size_t record_index = 0; record_index < records.size();
         ++record_index) {
        const auto& record = records[record_index];
        if (!record.directory()) continue;
        accelerators.directory_signature_bits[record_index >> 6U] |=
            std::uint64_t{1} << (record_index & 63U);
        std::wstring path_scratch;
        accelerators.path_trigram_signatures.push_back(
            make_path_signature(record_path(record_index, path_scratch)));
    }
    accelerators.directory_signature_rank_prefix.resize(
        accelerators.directory_signature_bits.size() + 1U);
    for (std::size_t word = 0;
         word < accelerators.directory_signature_bits.size(); ++word) {
        accelerators.directory_signature_rank_prefix[word + 1U] =
            accelerators.directory_signature_rank_prefix[word] +
            static_cast<std::uint32_t>(std::popcount(
                accelerators.directory_signature_bits[word]));
    }

    const auto path_uses_parent = [&](std::size_t record_index,
                                      std::size_t parent_index) {
        const auto& record = records[record_index];
        std::wstring parent_scratch;
        std::wstring record_scratch;
        const auto parent_path = record_path(parent_index, parent_scratch);
        const auto path = record_path(record_index, record_scratch);
        const std::wstring_view name(strings.data() + record.name_offset(),
                                     record.name_length());
        if (path.size() < name.size() || !path.ends_with(name)) return false;
        const auto prefix_length = path.size() - name.size();
        if (parent_path.ends_with(L"\\") || parent_path.ends_with(L"/")) {
            return prefix_length == parent_path.size() &&
                path.starts_with(parent_path);
        }
        return prefix_length == parent_path.size() + 1 &&
            path.starts_with(parent_path) &&
            (path[parent_path.size()] == L'\\' ||
             path[parent_path.size()] == L'/');
    };

    for (std::size_t record_index = 0; record_index < records.size();
         ++record_index) {
        const auto& record = records[record_index];
        if (record.directory()) continue;
        if (record.parent_index != missing_parent_index &&
            record.parent_index < records.size() &&
            records[record.parent_index].directory() &&
            path_uses_parent(record_index, record.parent_index)) {
            continue;
        }
        if (accelerators.path_trigram_signatures.size() >=
            invalid_signature_owner) {
            throw std::length_error("path signature index exceeds 32 bits");
        }
        std::wstring path_scratch;
        const auto signature_index = static_cast<std::uint32_t>(
            accelerators.path_trigram_signatures.size());
        accelerators.path_trigram_signatures.push_back(
            make_path_signature(record_path(record_index, path_scratch)));
        accelerators.path_signature_fallbacks.push_back({
            static_cast<std::uint32_t>(record_index), signature_index});
    }
    accelerators.path_trigram_signatures.shrink_to_fit();

    const auto build_prefix_tables = [](
            std::vector<std::uint64_t>& entries,
            std::vector<std::uint32_t>& order,
            std::vector<NamePrefixRange>& ranges,
            std::vector<NameFirstCharacterRange>& first_character_ranges) {
        std::sort(entries.begin(), entries.end());
        order.reserve(entries.size());
        for (const auto entry : entries) {
            order.push_back(static_cast<std::uint32_t>(entry));
        }

        std::size_t begin = 0;
        while (begin < entries.size()) {
            const auto key = static_cast<std::uint32_t>(entries[begin] >> 32U);
            std::size_t end = begin + 1;
            while (end < entries.size() &&
                   static_cast<std::uint32_t>(entries[end] >> 32U) == key) {
                ++end;
            }
            ranges.push_back({key, static_cast<std::uint32_t>(begin),
                              static_cast<std::uint32_t>(end)});
            begin = end;
        }

        begin = 0;
        while (begin < entries.size()) {
            const auto key = static_cast<std::uint16_t>(entries[begin] >> 48U);
            std::size_t end = begin + 1;
            while (end < entries.size() &&
                   static_cast<std::uint16_t>(entries[end] >> 48U) == key) {
                ++end;
            }
            first_character_ranges.push_back({
                key, static_cast<std::uint32_t>(begin),
                static_cast<std::uint32_t>(end)});
            begin = end;
        }
    };
    build_prefix_tables(prefix_entries, accelerators.prefix_order,
                        accelerators.prefix_ranges,
                        accelerators.first_character_ranges);
    build_prefix_tables(folded_prefix_entries,
                        accelerators.folded_prefix_order,
                        accelerators.folded_prefix_ranges,
                        accelerators.folded_first_character_ranges);

    accelerators.natural_name_order.resize(records.size());
    std::iota(accelerators.natural_name_order.begin(),
              accelerators.natural_name_order.end(), std::uint32_t{});
    std::vector<std::uint64_t> natural_name_prefixes(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        natural_name_prefixes[index] = natural_name_sort_prefix(
            std::wstring_view(strings.data() + record.name_offset(),
                              record.name_length()));
    }
    const auto natural_name_before =
        [&](std::uint32_t left_index, std::uint32_t right_index) {
            const auto left_prefix = natural_name_prefixes[left_index];
            const auto right_prefix = natural_name_prefixes[right_index];
            if (((left_prefix | right_prefix) & unsafe_natural_prefix) == 0 &&
                left_prefix != right_prefix) {
                return left_prefix < right_prefix;
            }
            const auto& left = records[left_index];
            const auto& right = records[right_index];
            const std::wstring_view left_name(
                strings.data() + left.name_offset(), left.name_length());
            const std::wstring_view right_name(
                strings.data() + right.name_offset(), right.name_length());
            int order = natural_compare(left_name, right_name, false);
            if (!order) {
                std::wstring left_path_scratch;
                std::wstring right_path_scratch;
                const auto left_path =
                    record_path(left_index, left_path_scratch);
                const auto right_path =
                    record_path(right_index, right_path_scratch);
                order = natural_compare(left_path, right_path, false);
            }
            if (!order) {
                order = left.id < right.id
                    ? -1 : (left.id > right.id ? 1 : 0);
            }
            return order < 0;
        };

    // The three-token prefix is monotonic with natural_compare. An LSD
    // radix pass puts different prefixes in final order in linear time; only
    // equal-prefix runs need the expensive variable-length comparator.
    std::vector<std::uint32_t> unsafe_order;
    unsafe_order.reserve(64);
    std::size_t safe_count = 0;
    for (const auto index : accelerators.natural_name_order) {
        if (natural_name_prefixes[index] & unsafe_natural_prefix) {
            unsafe_order.push_back(index);
        } else {
            accelerators.natural_name_order[safe_count++] = index;
        }
    }
    accelerators.natural_name_order.resize(safe_count);

    constexpr std::size_t radix_bits = 17;
    constexpr std::size_t radix_size = std::size_t{1} << radix_bits;
    constexpr std::uint64_t radix_mask = radix_size - 1;
    std::vector<std::uint32_t> radix_buffer(safe_count);
    std::vector<std::size_t> counts(radix_size);
    for (unsigned shift : {0U, 17U, 34U}) {
        std::fill(counts.begin(), counts.end(), 0);
        for (const auto index : accelerators.natural_name_order) {
            ++counts[(natural_name_prefixes[index] >> shift) & radix_mask];
        }
        std::size_t position = 0;
        for (auto& count : counts) {
            const auto bucket_size = count;
            count = position;
            position += bucket_size;
        }
        for (const auto index : accelerators.natural_name_order) {
            radix_buffer[counts[(natural_name_prefixes[index] >> shift) &
                                radix_mask]++] = index;
        }
        accelerators.natural_name_order.swap(radix_buffer);
    }

    std::vector<std::pair<std::size_t, std::size_t>> equal_prefix_runs;
    for (std::size_t begin = 0; begin < safe_count;) {
        std::size_t end = begin + 1;
        const auto key = natural_name_prefixes[
            accelerators.natural_name_order[begin]];
        while (end < safe_count && natural_name_prefixes[
                   accelerators.natural_name_order[end]] == key) {
            ++end;
        }
        if (end - begin > 1) equal_prefix_runs.emplace_back(begin, end);
        begin = end;
    }

    const std::size_t hardware_threads =
        std::max(1U, std::thread::hardware_concurrency());
    const std::size_t sort_threads = safe_count >= 262'144
        ? std::min<std::size_t>(hardware_threads, 16) : 1;
    if (sort_threads == 1 || equal_prefix_runs.size() < 2) {
        for (const auto [begin, end] : equal_prefix_runs) {
            std::sort(accelerators.natural_name_order.begin() + begin,
                      accelerators.natural_name_order.begin() + end,
                      natural_name_before);
        }
    } else {
        std::atomic_size_t next_run{};
        std::vector<std::thread> workers;
        workers.reserve(sort_threads);
        for (std::size_t worker = 0; worker < sort_threads; ++worker) {
            workers.emplace_back([&] {
                for (;;) {
                    const auto run = next_run.fetch_add(1,
                        std::memory_order_relaxed);
                    if (run >= equal_prefix_runs.size()) break;
                    const auto [begin, end] = equal_prefix_runs[run];
                    std::sort(
                        accelerators.natural_name_order.begin() + begin,
                        accelerators.natural_name_order.begin() + end,
                        natural_name_before);
                }
            });
        }
        for (auto& worker : workers) worker.join();
    }

    if (!unsafe_order.empty()) {
        std::sort(unsafe_order.begin(), unsafe_order.end(), natural_name_before);
        std::vector<std::uint32_t> merged;
        merged.reserve(records.size());
        std::merge(accelerators.natural_name_order.begin(),
                   accelerators.natural_name_order.end(),
                   unsafe_order.begin(), unsafe_order.end(),
                   std::back_inserter(merged), natural_name_before);
        accelerators.natural_name_order.swap(merged);
    }

    // A Bloom signature can reject most names, but a rare query still has to
    // touch every record. Each 16-bit trigram hash therefore points at natural
    // name-order positions. The positions inside a bucket are monotonic and
    // are written directly as unsigned delta/varints, avoiding the previous
    // full uint32 posting_positions build buffer.
    std::vector<std::uint32_t> posting_counts(name_trigram_bucket_count);
    std::vector<std::uint32_t> posting_encoded_sizes(
        name_trigram_bucket_count);
    std::vector<std::uint32_t> previous_positions(
        name_trigram_bucket_count);
    std::vector<std::uint32_t> seen(name_trigram_bucket_count);
    std::uint32_t generation = 0;
    const auto visit_record_trigram_buckets =
        [&](std::uint32_t record_index, auto&& visitor) {
            if (++generation == 0) {
                std::fill(seen.begin(), seen.end(), 0);
                generation = 1;
            }
            const auto visit_value = [&](std::wstring_view value) {
                for (std::size_t offset = 0; offset + 2 < value.size();
                     ++offset) {
                    const auto bucket = name_trigram_key(
                        value[offset], value[offset + 1], value[offset + 2]) &
                        name_trigram_bucket_mask;
                    if (seen[bucket] == generation) continue;
                    seen[bucket] = generation;
                    visitor(bucket);
                }
            };

            const auto& record = records[record_index];
            const std::wstring_view name(strings.data() + record.name_offset(),
                                         record.name_length());
            visit_value(name);
            if (std::any_of(name.begin(), name.end(),
                            [](wchar_t ch) { return ch >= 0x80; })) {
                const auto folded = normalize_match_text(name, false, false);
                bool differs = folded.size() != name.size();
                if (!differs) {
                    for (std::size_t offset = 0; offset < name.size(); ++offset) {
                        if (folded[offset] !=
                            normalize_char(name[offset], false)) {
                            differs = true;
                            break;
                        }
                    }
                }
                if (differs) visit_value(folded);
            }
        };
    const auto varint_size = [](std::uint32_t value) {
        std::uint32_t bytes = 1;
        while (value >= 0x80U) {
            value >>= 7U;
            ++bytes;
        }
        return bytes;
    };

    for (std::uint32_t natural_position = 0;
         natural_position < accelerators.natural_name_order.size();
         ++natural_position) {
        const auto record_index =
            accelerators.natural_name_order[natural_position];
        visit_record_trigram_buckets(record_index, [&](std::uint32_t bucket) {
            if (posting_counts[bucket] ==
                std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("name trigram posting bucket overflow");
            }
            const auto delta = natural_position - previous_positions[bucket];
            const auto bytes = varint_size(delta);
            if (posting_encoded_sizes[bucket] >
                std::numeric_limits<std::uint32_t>::max() - bytes) {
                throw std::length_error(
                    "name trigram posting bucket exceeds 32-bit bytes");
            }
            posting_encoded_sizes[bucket] += bytes;
            previous_positions[bucket] = natural_position;
            ++posting_counts[bucket];
        });
    }

    auto& postings = accelerators.trigram_postings;
    postings.byte_offsets.resize(name_trigram_bucket_count + 1);
    postings.counts = posting_counts;
    std::uint64_t encoded_size = 0;
    for (std::size_t bucket = 0; bucket < name_trigram_bucket_count; ++bucket) {
        postings.byte_offsets[bucket] =
            static_cast<std::uint32_t>(encoded_size);
        encoded_size += posting_encoded_sizes[bucket];
        if (encoded_size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error(
                "compressed name trigram postings exceed 32-bit byte offsets");
        }
    }
    postings.byte_offsets.back() = static_cast<std::uint32_t>(encoded_size);
    postings.encoded_positions.resize(static_cast<std::size_t>(encoded_size));

    auto byte_cursors = postings.byte_offsets;
    std::fill(previous_positions.begin(), previous_positions.end(), 0);
    std::fill(seen.begin(), seen.end(), 0);
    generation = 0;
    const auto write_varint = [&](std::uint32_t bucket, std::uint32_t value) {
        auto& cursor = byte_cursors[bucket];
        while (value >= 0x80U) {
            postings.encoded_positions[cursor++] =
                static_cast<std::uint8_t>((value & 0x7fU) | 0x80U);
            value >>= 7U;
        }
        postings.encoded_positions[cursor++] =
            static_cast<std::uint8_t>(value);
    };
    for (std::uint32_t natural_position = 0;
         natural_position < accelerators.natural_name_order.size();
         ++natural_position) {
        const auto record_index =
            accelerators.natural_name_order[natural_position];
        visit_record_trigram_buckets(record_index, [&](std::uint32_t bucket) {
            write_varint(bucket,
                         natural_position - previous_positions[bucket]);
            previous_positions[bucket] = natural_position;
        });
    }
    for (std::size_t bucket = 0; bucket < name_trigram_bucket_count; ++bucket) {
        if (byte_cursors[bucket] != postings.byte_offsets[bucket + 1]) {
            throw std::runtime_error("name trigram posting encode mismatch");
        }
    }
    return accelerators;
}

void MetadataIndex::compact_base_paths(
    std::vector<CompactRecord>& records,
    std::vector<wchar_t>& strings) {
    std::vector<std::uint8_t> name_only(records.size());
    std::size_t compact_characters = 0;

    const auto stored_path = [&](const CompactRecord& record) {
        return std::wstring_view(strings.data() + record.path_offset,
                                 record.path_length);
    };
    const auto stored_name = [&](const CompactRecord& record) {
        return std::wstring_view(strings.data() + record.name_offset(),
                                 record.name_length());
    };
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        bool can_store_name_only = false;
        if (record.parent_index != missing_parent_index &&
            record.parent_index < records.size() &&
            record.parent_index != index) {
            const auto& parent = records[record.parent_index];
            if (parent.directory()) {
                const auto parent_path = stored_path(parent);
                const auto path = stored_path(record);
                const auto name = stored_name(record);
                if (path.size() >= name.size() && path.ends_with(name)) {
                    const auto prefix_length = path.size() - name.size();
                    if (parent_path.ends_with(L"\\") ||
                        parent_path.ends_with(L"/")) {
                        can_store_name_only =
                            prefix_length == parent_path.size() &&
                            path.starts_with(parent_path);
                    } else {
                        can_store_name_only =
                            prefix_length == parent_path.size() + 1 &&
                            path.starts_with(parent_path) &&
                            (path[parent_path.size()] == L'\\' ||
                             path[parent_path.size()] == L'/');
                    }
                }
            }
        }
        name_only[index] = can_store_name_only ? 1 : 0;
        compact_characters += can_store_name_only
            ? record.name_length() : record.path_length;
    }
    if (compact_characters > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("compact string arena exceeds 32-bit offsets");
    }

    std::vector<wchar_t> compact_strings;
    compact_strings.reserve(compact_characters);
    for (std::size_t index = 0; index < records.size(); ++index) {
        auto& record = records[index];
        const auto path = stored_path(record);
        const auto name = stored_name(record);
        const auto offset = static_cast<std::uint32_t>(compact_strings.size());
        if (name_only[index] != 0) {
            compact_strings.insert(compact_strings.end(), name.begin(),
                                   name.end());
            record.path_offset = offset;
            record.path_length = static_cast<std::uint32_t>(name.size());
            record.name_only_path = true;
        } else {
            compact_strings.insert(compact_strings.end(), path.begin(),
                                   path.end());
            record.path_offset = offset;
            record.path_length = static_cast<std::uint32_t>(path.size());
            record.name_only_path = false;
        }
    }
    strings.swap(compact_strings);
}

void MetadataIndex::finalize_pending_records(
    std::vector<PendingCompactRecord>& pending,
    std::vector<CompactRecord>& records,
    std::vector<ParentIdAnchor>& parent_id_anchors) {
    if (pending.size() >= missing_parent_index) {
        throw std::length_error("compact parent index exceeds 31 bits");
    }
    std::sort(pending.begin(), pending.end(),
              [](const PendingCompactRecord& a,
                 const PendingCompactRecord& b) {
                  return a.record.id < b.record.id;
              });
    records.clear();
    records.reserve(pending.size());
    parent_id_anchors.clear();
    for (std::size_t index = 0; index < pending.size(); ++index) {
        auto& item = pending[index];
        const auto parent = std::lower_bound(
            pending.begin(), pending.end(), item.parent_id,
            [](const PendingCompactRecord& candidate, std::uint64_t id) {
                return candidate.record.id < id;
            });
        if (parent != pending.end() && parent->record.id == item.parent_id) {
            item.record.parent_index = static_cast<std::uint32_t>(
                std::distance(pending.begin(), parent));
        } else {
            item.record.parent_index = missing_parent_index;
            if (item.parent_id != 0) {
                parent_id_anchors.push_back({item.record.id, item.parent_id});
            }
        }
        records.push_back(item.record);
    }
}

void MetadataIndex::replace(const std::vector<FileRecord>& source) {
    replace_impl(source, nullptr);
}

void MetadataIndex::replace(std::vector<FileRecord>&& source) {
    replace_impl(source, &source);
}

void MetadataIndex::replace_impl(
    const std::vector<FileRecord>& source,
    std::vector<FileRecord>* consumable_source) {
    std::size_t total_chars = 0;
    for (const auto& item : source) total_chars += item.path.size();
    if (total_chars > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("string arena exceeds 32-bit offsets");
    }
    std::vector<PendingCompactRecord> pending;
    std::vector<CompactRecord> records;
    std::vector<ParentIdAnchor> parent_id_anchors;
    std::vector<wchar_t> strings;
    pending.reserve(source.size());
    strings.reserve(total_chars);
    for (const auto& item : source) {
        if (item.path.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("path exceeds compact 16-bit length");
        }
        PendingCompactRecord pending_record;
        auto& record = pending_record.record;
        record.id = item.id;
        pending_record.parent_id = item.parent_id;
        record.size = item.size;
        record.last_write_time = item.last_write_time;
        record.attributes = item.directory
            ? item.attributes | 0x10U : item.attributes & ~0x10U;
        record.path_offset = static_cast<std::uint32_t>(strings.size());
        record.path_length = static_cast<std::uint32_t>(item.path.size());
        strings.insert(strings.end(), item.path.begin(), item.path.end());
        std::size_t relative_name = item.path.size();
        if (!item.name.empty() && item.path.size() >= item.name.size() &&
            item.path.ends_with(item.name)) {
            relative_name = item.path.size() - item.name.size();
        } else {
            const auto slash = item.path.find_last_of(L"\\/");
            relative_name = slash == std::wstring::npos ? 0 : slash + 1;
        }
        const auto name_length = item.path.size() - relative_name;
        if (name_length > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("name exceeds compact 16-bit length");
        }
        record.name_length_value = static_cast<std::uint32_t>(name_length);
        pending.push_back(pending_record);
    }
    finalize_pending_records(pending, records, parent_id_anchors);
    if (consumable_source != nullptr) {
        // The compact records and path arena now contain everything needed by
        // the index. Release millions of source wstrings before allocating the
        // trigram postings and sort accelerators so they do not overlap at the
        // memory peak or remain alive for the lifetime of the service.
        std::vector<FileRecord>().swap(*consumable_source);
    }
    // Build accelerators from complete paths, then componentize the resident
    // arena. The owner metadata records only exceptional non-parent paths.
    auto name_accelerators = build_name_search_accelerators(records, strings);
    compact_base_paths(records, strings);
    std::unordered_map<std::uint64_t, FileRecord> old_overlay;
    std::unordered_set<std::uint64_t> old_removed;
    std::vector<std::uint64_t> old_suppressed_base_ids;
    {
        std::unique_lock lock(mutex_);
        // Swap in the fully-built index while holding the lock, then destroy
        // the previous multi-million-record buffers after readers can resume.
        records_.swap(records);
        parent_id_anchors_.swap(parent_id_anchors);
        strings_.swap(strings);
        name_bigram_signatures_.swap(name_accelerators.bigram_signatures);
        std::swap(name_trigram_postings_, name_accelerators.trigram_postings);
        path_trigram_signatures_.swap(
            name_accelerators.path_trigram_signatures);
        directory_signature_bits_.swap(
            name_accelerators.directory_signature_bits);
        directory_signature_rank_prefix_.swap(
            name_accelerators.directory_signature_rank_prefix);
        path_signature_fallbacks_.swap(
            name_accelerators.path_signature_fallbacks);
        natural_name_order_.swap(name_accelerators.natural_name_order);
        name_prefix_order_.swap(name_accelerators.prefix_order);
        name_prefix_ranges_.swap(name_accelerators.prefix_ranges);
        name_first_character_ranges_.swap(
            name_accelerators.first_character_ranges);
        folded_name_prefix_order_.swap(
            name_accelerators.folded_prefix_order);
        folded_name_prefix_ranges_.swap(
            name_accelerators.folded_prefix_ranges);
        folded_name_first_character_ranges_.swap(
            name_accelerators.folded_first_character_ranges);
        overlay_.swap(old_overlay);
        removed_.swap(old_removed);
        suppressed_base_ids_.swap(old_suppressed_base_ids);
        live_size_ = records_.size();
    }
}

bool MetadataIndex::base_contains(std::uint64_t id) const {
    const auto found = std::lower_bound(
        records_.begin(), records_.end(), id,
        [](const CompactRecord& record, std::uint64_t value) {
            return record.id < value;
        });
    return found != records_.end() && found->id == id;
}

std::uint32_t MetadataIndex::directory_signature_index(
    std::size_t record_index) const noexcept {
    constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
    const auto word_index = record_index >> 6U;
    if (word_index >= directory_signature_bits_.size() ||
        word_index >= directory_signature_rank_prefix_.size()) {
        return invalid;
    }
    const auto bit_index = static_cast<unsigned>(record_index & 63U);
    const auto word = directory_signature_bits_[word_index];
    const auto bit = std::uint64_t{1} << bit_index;
    if ((word & bit) == 0) return invalid;
    const auto lower_mask = bit_index == 0 ? std::uint64_t{} : bit - 1U;
    return directory_signature_rank_prefix_[word_index] +
        static_cast<std::uint32_t>(std::popcount(word & lower_mask));
}

std::uint32_t MetadataIndex::path_signature_owner(
    std::size_t record_index) const noexcept {
    constexpr auto invalid = std::numeric_limits<std::uint32_t>::max();
    if (record_index >= records_.size()) return invalid;
    const auto& record = records_[record_index];
    if (record.directory()) {
        return directory_signature_index(record_index);
    }
    if (record.name_only_path &&
        record.parent_index != missing_parent_index &&
        record.parent_index < records_.size() &&
        records_[record.parent_index].directory()) {
        return directory_signature_index(record.parent_index);
    }
    const auto found = std::lower_bound(
        path_signature_fallbacks_.begin(), path_signature_fallbacks_.end(),
        static_cast<std::uint32_t>(record_index),
        [](const PathSignatureFallback& fallback, std::uint32_t index) {
            return fallback.record_index < index;
        });
    return found != path_signature_fallbacks_.end() &&
           found->record_index == record_index
        ? found->signature_index : invalid;
}

void MetadataIndex::apply_delta(
    std::vector<FileRecord> upserts,
    const std::vector<std::uint64_t>& removed_ids) {
    std::unique_lock lock(mutex_);
    for (const auto id : removed_ids) {
        const bool in_overlay = overlay_.find(id) != overlay_.end();
        const bool in_base = base_contains(id);
        const bool live = in_overlay ||
                          (in_base && removed_.find(id) == removed_.end());
        overlay_.erase(id);
        if (in_base) removed_.insert(id);
        else removed_.erase(id);
        if (live) --live_size_;
    }
    for (auto& record : upserts) {
        const auto id = record.id;
        const bool in_overlay = overlay_.find(id) != overlay_.end();
        const bool in_base = base_contains(id);
        const bool live = in_overlay ||
                          (in_base && removed_.find(id) == removed_.end());
        removed_.erase(id);
        overlay_.insert_or_assign(id, std::move(record));
        if (!live) ++live_size_;
    }
    if (auto_compaction_threshold_ != 0 &&
        overlay_.size() + removed_.size() >= auto_compaction_threshold_) {
        compact_locked();
    } else {
        rebuild_suppressed_base_ids_locked();
    }
}

void MetadataIndex::apply_ntfs_changes(
    std::wstring_view volume_identity,
    std::wstring_view volume_root,
    std::uint64_t root_id,
    const UsnChangeBatch& batch) {
    const auto scoped_root_id =
        namespace_ntfs_file_id(volume_identity, root_id);
    std::unique_lock lock(mutex_);

    const auto base_record = [&](std::uint64_t id) -> const CompactRecord* {
        const auto found = std::lower_bound(
            records_.begin(), records_.end(), id,
            [](const CompactRecord& record, std::uint64_t value) {
                return record.id < value;
            });
        return found != records_.end() && found->id == id ? &*found : nullptr;
    };
    const auto live_record = [&](std::uint64_t id,
                                 FileRecord& result) -> bool {
        if (const auto found = overlay_.find(id); found != overlay_.end()) {
            result = found->second;
            return true;
        }
        if (removed_.find(id) != removed_.end()) return false;
        const auto* base = base_record(id);
        if (base == nullptr) return false;
        result = materialize(*base);
        return true;
    };
    const auto parent_id_of = [&](std::uint64_t id,
                                  std::uint64_t& parent_id) -> bool {
        if (const auto found = overlay_.find(id); found != overlay_.end()) {
            parent_id = found->second.parent_id;
            return true;
        }
        if (removed_.find(id) != removed_.end()) return false;
        const auto* base = base_record(id);
        if (base == nullptr) return false;
        parent_id = this->parent_id(*base);
        return true;
    };
    const auto promote = [&](std::uint64_t id) -> FileRecord& {
        if (auto found = overlay_.find(id); found != overlay_.end()) {
            return found->second;
        }
        const auto* base = base_record(id);
        if (base == nullptr || removed_.find(id) != removed_.end()) {
            throw std::logic_error("cannot promote missing index record");
        }
        auto [inserted, created] = overlay_.emplace(id, materialize(*base));
        (void)created;
        return inserted->second;
    };

    std::unordered_set<std::uint64_t> pending_renames;
    std::unordered_set<std::uint64_t> changed_ids;
    std::unordered_set<std::uint64_t> path_roots;
    pending_renames.reserve(batch.changes.size() / 8 + 1);
    changed_ids.reserve(batch.changes.size() / 2 + 1);

    for (const auto& raw_change : batch.changes) {
        const auto id = namespace_ntfs_file_id(
            volume_identity, raw_change.file_id);
        const auto parent_id = namespace_ntfs_file_id(
            volume_identity, raw_change.parent_id);
        if ((raw_change.reason & USN_REASON_RENAME_OLD_NAME) != 0) {
            pending_renames.insert(id);
            continue;
        }

        if ((raw_change.reason & USN_REASON_FILE_DELETE) != 0) {
            FileRecord deleting;
            if (live_record(id, deleting)) {
                if (deleting.directory) path_roots.insert(id);
                const bool in_base = base_record(id) != nullptr;
                overlay_.erase(id);
                if (in_base) removed_.insert(id);
                else removed_.erase(id);
                --live_size_;
            }
            pending_renames.erase(id);
            changed_ids.erase(id);
            continue;
        }

        const bool create =
            (raw_change.reason & USN_REASON_FILE_CREATE) != 0;
        const bool rename_new =
            (raw_change.reason & USN_REASON_RENAME_NEW_NAME) != 0;
        FileRecord existing;
        const bool found = live_record(id, existing);
        if (!found) {
            if (!create && !rename_new) continue;
            FileRecord record;
            record.id = id;
            record.parent_id = parent_id;
            record.attributes = raw_change.attributes;
            record.directory =
                (raw_change.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            record.name = raw_change.name;
            removed_.erase(id);
            overlay_.insert_or_assign(id, std::move(record));
            changed_ids.insert(id);
            ++live_size_;
            pending_renames.erase(id);
            continue;
        }

        auto& record = promote(id);
        if (rename_new) {
            record.parent_id = parent_id;
            record.name = raw_change.name;
            if (record.directory) path_roots.insert(id);
            pending_renames.erase(id);
        } else if (create) {
            record.parent_id = parent_id;
            record.name = raw_change.name;
        }
        record.attributes = raw_change.attributes;
        record.directory =
            (raw_change.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        changed_ids.insert(id);
    }

    std::unordered_map<std::uint64_t, std::wstring> paths;
    paths.reserve(changed_ids.size() * 2 + path_roots.size() + 8);
    paths.emplace(scoped_root_id, std::wstring(volume_root));
    std::unordered_set<std::uint64_t> resolving;
    resolving.reserve(64);
    std::function<std::wstring(std::uint64_t, unsigned)> resolve =
        [&](std::uint64_t id, unsigned depth) -> std::wstring {
        if (id == scoped_root_id) return std::wstring(volume_root);
        if (const auto cached = paths.find(id); cached != paths.end()) {
            return cached->second;
        }
        if (depth > 512 || !resolving.insert(id).second) {
            return std::wstring(volume_root) + L"\\$Cycle";
        }
        FileRecord record;
        if (!live_record(id, record)) {
            resolving.erase(id);
            return std::wstring(volume_root) + L"\\$OrphanFiles";
        }
        auto path = resolve(record.parent_id, depth + 1);
        if (!path.ends_with(L"\\") && !path.ends_with(L"/")) {
            path.push_back(L'\\');
        }
        path += record.name;
        resolving.erase(id);
        paths.emplace(id, path);
        return path;
    };
    const auto refresh_path = [&](FileRecord& record) {
        record.path = resolve(record.parent_id, 0);
        if (!record.path.ends_with(L"\\") &&
            !record.path.ends_with(L"/")) {
            record.path.push_back(L'\\');
        }
        record.path += record.name;
        paths.insert_or_assign(record.id, record.path);
    };

    std::unordered_map<std::uint64_t, bool> affected_cache;
    affected_cache.reserve(path_roots.size() * 4 + 64);
    affected_cache.emplace(scoped_root_id, false);
    for (const auto id : path_roots) affected_cache.insert_or_assign(id, true);
    std::unordered_set<std::uint64_t> resolving_affected;
    resolving_affected.reserve(64);
    std::function<bool(std::uint64_t, unsigned)> is_path_affected =
        [&](std::uint64_t id, unsigned depth) -> bool {
        if (const auto cached = affected_cache.find(id);
            cached != affected_cache.end()) {
            return cached->second;
        }
        if (depth > 512 || !resolving_affected.insert(id).second) return false;
        std::uint64_t parent_id = 0;
        bool affected = false;
        if (parent_id_of(id, parent_id) && parent_id != id) {
            affected = is_path_affected(parent_id, depth + 1);
        }
        resolving_affected.erase(id);
        affected_cache.emplace(id, affected);
        return affected;
    };

    if (!path_roots.empty()) {
        for (const auto& base : records_) {
            if (removed_.find(base.id) != removed_.end()) continue;
            if (changed_ids.find(base.id) != changed_ids.end() ||
                is_path_affected(base.id, 0)) {
                refresh_path(promote(base.id));
            }
        }
        for (auto& [id, record] : overlay_) {
            if (changed_ids.find(id) != changed_ids.end() ||
                is_path_affected(id, 0)) {
                refresh_path(record);
            }
        }
    } else {
        for (const auto id : changed_ids) {
            if (auto found = overlay_.find(id); found != overlay_.end()) {
                refresh_path(found->second);
            }
        }
    }

    // USN records carry names, parent IDs, attributes, and reasons, but not
    // file size or timestamps. Copy only directly changed records after their
    // current paths have been resolved, release the index lock for filesystem
    // I/O, then merge metadata back only when the record still has that path.
    // Directory rename descendants keep their existing metadata and only
    // receive refreshed paths above.
    std::vector<FileRecord> metadata_updates;
    metadata_updates.reserve(changed_ids.size());
    for (const auto id : changed_ids) {
        if (const auto found = overlay_.find(id); found != overlay_.end()) {
            metadata_updates.push_back(found->second);
        }
    }
    if (!metadata_updates.empty()) {
        lock.unlock();
        metadata_updates.erase(
            std::remove_if(metadata_updates.begin(), metadata_updates.end(),
                           [](FileRecord& record) {
                               return !hydrate_file_metadata(record);
                           }),
            metadata_updates.end());
        lock.lock();
        for (const auto& update : metadata_updates) {
            const auto found = overlay_.find(update.id);
            if (found == overlay_.end() || found->second.path != update.path) {
                continue;
            }
            auto& record = found->second;
            record.size = update.size;
            record.creation_time = update.creation_time;
            record.last_access_time = update.last_access_time;
            record.last_write_time = update.last_write_time;
            record.change_time = update.change_time;
            record.attributes = update.attributes;
            record.directory = update.directory;
        }
    }

    if (auto_compaction_threshold_ != 0 &&
        overlay_.size() + removed_.size() >= auto_compaction_threshold_) {
        compact_locked();
    } else {
        rebuild_suppressed_base_ids_locked();
    }
}

void MetadataIndex::rebuild_suppressed_base_ids_locked() {
    suppressed_base_ids_.clear();
    suppressed_base_ids_.reserve(overlay_.size() + removed_.size());
    for (const auto id : removed_) {
        suppressed_base_ids_.push_back(id);
    }
    for (const auto& [id, record] : overlay_) {
        (void)record;
        if (base_contains(id)) suppressed_base_ids_.push_back(id);
    }
    std::sort(suppressed_base_ids_.begin(), suppressed_base_ids_.end());
    suppressed_base_ids_.erase(
        std::unique(suppressed_base_ids_.begin(),
                    suppressed_base_ids_.end()),
        suppressed_base_ids_.end());
}

void MetadataIndex::compact_locked() {
    if (overlay_.empty() && removed_.empty()) return;

    std::size_t total_chars = 0;
    std::wstring path_scratch;
    for (const auto& record : records_) {
        if (removed_.find(record.id) == removed_.end() &&
            overlay_.find(record.id) == overlay_.end()) {
            total_chars += path_view(record, path_scratch).size();
        }
    }
    for (const auto& [id, record] : overlay_) {
        (void)id;
        total_chars += record.path.size();
    }
    if (total_chars > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("string arena exceeds 32-bit offsets");
    }

    std::vector<PendingCompactRecord> pending;
    std::vector<CompactRecord> compacted;
    std::vector<ParentIdAnchor> compacted_parent_id_anchors;
    std::vector<wchar_t> strings;
    pending.reserve(live_size_);
    strings.reserve(total_chars);

    for (const auto& source : records_) {
        if (removed_.find(source.id) != removed_.end() ||
            overlay_.find(source.id) != overlay_.end()) {
            continue;
        }
        const auto path = path_view(source, path_scratch);
        if (path.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("path exceeds compact 16-bit length");
        }
        PendingCompactRecord pending_record;
        pending_record.record = source;
        pending_record.parent_id = parent_id(source);
        auto& record = pending_record.record;
        record.path_offset = static_cast<std::uint32_t>(strings.size());
        record.path_length = static_cast<std::uint32_t>(path.size());
        record.name_length_value = source.name_length();
        record.name_only_path = false;
        strings.insert(strings.end(), path.begin(), path.end());
        pending.push_back(pending_record);
    }

    for (const auto& [id, source] : overlay_) {
        (void)id;
        if (source.path.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("path exceeds compact 16-bit length");
        }
        PendingCompactRecord pending_record;
        auto& record = pending_record.record;
        record.id = source.id;
        pending_record.parent_id = source.parent_id;
        record.size = source.size;
        record.last_write_time = source.last_write_time;
        record.attributes = source.directory
            ? source.attributes | 0x10U : source.attributes & ~0x10U;
        record.path_offset = static_cast<std::uint32_t>(strings.size());
        record.path_length = static_cast<std::uint32_t>(source.path.size());
        strings.insert(strings.end(), source.path.begin(), source.path.end());
        std::size_t relative_name = source.path.size();
        if (!source.name.empty() && source.path.size() >= source.name.size() &&
            source.path.ends_with(source.name)) {
            relative_name = source.path.size() - source.name.size();
        } else {
            const auto slash = source.path.find_last_of(L"\\/");
            relative_name = slash == std::wstring::npos ? 0 : slash + 1;
        }
        const auto name_length = source.path.size() - relative_name;
        if (name_length > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("name exceeds compact 16-bit length");
        }
        record.name_length_value = static_cast<std::uint32_t>(name_length);
        pending.push_back(pending_record);
    }

    finalize_pending_records(pending, compacted,
                             compacted_parent_id_anchors);
    compact_base_paths(compacted, strings);
    auto name_accelerators = build_name_search_accelerators(compacted, strings);
    records_ = std::move(compacted);
    parent_id_anchors_ = std::move(compacted_parent_id_anchors);
    strings_ = std::move(strings);
    name_bigram_signatures_ = std::move(name_accelerators.bigram_signatures);
    name_trigram_postings_ = std::move(name_accelerators.trigram_postings);
    path_trigram_signatures_ =
        std::move(name_accelerators.path_trigram_signatures);
    directory_signature_bits_ =
        std::move(name_accelerators.directory_signature_bits);
    directory_signature_rank_prefix_ =
        std::move(name_accelerators.directory_signature_rank_prefix);
    path_signature_fallbacks_ =
        std::move(name_accelerators.path_signature_fallbacks);
    natural_name_order_ = std::move(name_accelerators.natural_name_order);
    name_prefix_order_ = std::move(name_accelerators.prefix_order);
    name_prefix_ranges_ = std::move(name_accelerators.prefix_ranges);
    name_first_character_ranges_ =
        std::move(name_accelerators.first_character_ranges);
    folded_name_prefix_order_ =
        std::move(name_accelerators.folded_prefix_order);
    folded_name_prefix_ranges_ =
        std::move(name_accelerators.folded_prefix_ranges);
    folded_name_first_character_ranges_ =
        std::move(name_accelerators.folded_first_character_ranges);
    overlay_.clear();
    removed_.clear();
    suppressed_base_ids_.clear();
    live_size_ = records_.size();
    ++compaction_count_;
}

bool MetadataIndex::compact() {
    std::unique_lock lock(mutex_);
    if (overlay_.empty() && removed_.empty()) return false;
    compact_locked();
    return true;
}

void MetadataIndex::set_auto_compaction_threshold(std::size_t threshold) {
    std::unique_lock lock(mutex_);
    auto_compaction_threshold_ = threshold;
    if (threshold != 0 && overlay_.size() + removed_.size() >= threshold) {
        compact_locked();
    }
}

std::size_t MetadataIndex::compaction_count() const {
    std::shared_lock lock(mutex_);
    return compaction_count_;
}

MetadataIndexStorageStats MetadataIndex::storage_stats() const {
    std::shared_lock lock(mutex_);
    MetadataIndexStorageStats stats;
    stats.base_records = records_.size();
    stats.name_only_paths = static_cast<std::size_t>(std::count_if(
        records_.begin(), records_.end(),
        [](const CompactRecord& record) { return record.name_only_path; }));
    stats.string_characters = strings_.size();
    stats.record_bytes = records_.capacity() * sizeof(CompactRecord) +
        parent_id_anchors_.capacity() * sizeof(ParentIdAnchor);
    stats.string_bytes = strings_.capacity() * sizeof(wchar_t);
    stats.path_signature_count = path_trigram_signatures_.size();
    stats.path_signature_owner_bytes =
        directory_signature_bits_.capacity() * sizeof(std::uint64_t) +
        directory_signature_rank_prefix_.capacity() * sizeof(std::uint32_t) +
        path_signature_fallbacks_.capacity() * sizeof(PathSignatureFallback);
    stats.signature_bytes =
        name_bigram_signatures_.capacity() * sizeof(NameBigramSignature) +
        path_trigram_signatures_.capacity() * sizeof(PathTrigramSignature) +
        stats.path_signature_owner_bytes;
    stats.posting_entries = std::accumulate(
        name_trigram_postings_.counts.begin(),
        name_trigram_postings_.counts.end(), std::size_t{});
    stats.posting_bytes =
        (name_trigram_postings_.byte_offsets.capacity() +
         name_trigram_postings_.counts.capacity()) * sizeof(std::uint32_t) +
        name_trigram_postings_.encoded_positions.capacity() *
            sizeof(std::uint8_t);
    stats.ordering_bytes =
        (natural_name_order_.capacity() + name_prefix_order_.capacity() +
         folded_name_prefix_order_.capacity()) * sizeof(std::uint32_t) +
        (name_prefix_ranges_.capacity() + folded_name_prefix_ranges_.capacity()) *
            sizeof(NamePrefixRange) +
        (name_first_character_ranges_.capacity() +
         folded_name_first_character_ranges_.capacity()) *
            sizeof(NameFirstCharacterRange);
    stats.total_base_capacity_bytes =
        stats.record_bytes + stats.string_bytes + stats.signature_bytes +
        stats.posting_bytes + stats.ordering_bytes;
    return stats;
}

std::wstring_view MetadataIndex::path_view(
    const CompactRecord& record, std::wstring& scratch) const {
    const auto stored_path = [&](const CompactRecord& item) {
        return std::wstring_view(strings_.data() + item.path_offset,
                                 item.path_length);
    };
    if (!record.name_only_path) return stored_path(record);

    // Componentized paths can include directory records too. Walk direct
    // parent indices to the nearest full-path anchor, then append names in
    // forward order. A fixed stack keeps normal path reconstruction free of
    // helper allocations; scratch owns only the final returned path.
    constexpr std::size_t maximum_components = 512;
    std::array<const CompactRecord*, maximum_components> components{};
    std::size_t component_count = 0;
    const CompactRecord* current = &record;
    while (current->name_only_path && component_count < components.size()) {
        components[component_count++] = current;
        if (current->parent_index == missing_parent_index ||
            current->parent_index >= records_.size()) {
            return stored_path(record);
        }
        const auto* parent = &records_[current->parent_index];
        if (parent == current) return stored_path(record);
        current = parent;
    }
    if (current->name_only_path) return stored_path(record);

    scratch.assign(stored_path(*current));
    while (component_count != 0) {
        const auto* component = components[--component_count];
        if (!scratch.ends_with(L"\\") && !scratch.ends_with(L"/")) {
            scratch.push_back(L'\\');
        }
        scratch.append(name_view(*component));
    }
    return scratch;
}

std::wstring_view MetadataIndex::name_view(
    const CompactRecord& record) const {
    return {strings_.data() + record.name_offset(), record.name_length()};
}

std::uint64_t MetadataIndex::parent_id(const CompactRecord& record) const {
    if (record.parent_index != missing_parent_index &&
        record.parent_index < records_.size()) {
        return records_[record.parent_index].id;
    }
    const auto found = std::lower_bound(
        parent_id_anchors_.begin(), parent_id_anchors_.end(), record.id,
        [](const ParentIdAnchor& anchor, std::uint64_t id) {
            return anchor.id < id;
        });
    return found != parent_id_anchors_.end() && found->id == record.id
        ? found->parent_id : 0;
}

FileRecord MetadataIndex::materialize(const CompactRecord& item) const {
    FileRecord result;
    std::wstring path_scratch;
    result.id = item.id;
    result.parent_id = parent_id(item);
    result.size = item.size;
    result.last_write_time = item.last_write_time;
    result.attributes = item.attributes;
    result.directory = item.directory();
    result.path.assign(path_view(item, path_scratch));
    result.name.assign(name_view(item));
    return result;
}

std::vector<SearchResult> MetadataIndex::search(
    std::wstring_view text, const SearchOptions& requested_options) const {
    const ParsedQuery query = parse_query(text);
    if (!query.valid) return {};
    SearchOptions options = requested_options;
    if (query.max_results.has_value()) {
        options.limit = std::min(options.limit, *query.max_results);
    }
    if (options.limit == 0) return {};
    if (query.terms.empty() && !query.directories_only.has_value()) return {};

    const bool sensitive = query.case_sensitive.value_or(options.case_sensitive);
    const bool whole_word = query.whole_word.value_or(options.whole_word);
    std::vector<CompiledTerm> terms;
    terms.reserve(query.terms.size());
    for (const auto& term : query.terms) {
        terms.push_back(compile_term(term, sensitive, options.match_diacritics));
        if (!terms.back().valid) return {};
    }
    const bool query_reads_path = options.match_path ||
        std::any_of(terms.begin(), terms.end(), [](const CompiledTerm& term) {
            if (term.target == MatchTarget::path ||
                term.target == MatchTarget::path_depth ||
                term.target == MatchTarget::parent_path) {
                return true;
            }
            return term.target == MatchTarget::filename_list &&
                std::any_of(term.alternatives.begin(), term.alternatives.end(),
                            [](const std::wstring& alternative) {
                                return alternative.find_first_of(L"\\:") !=
                                    std::wstring::npos;
                            });
        });
    const bool query_reads_child_counts =
        std::any_of(terms.begin(), terms.end(), [](const CompiledTerm& term) {
            return term.target == MatchTarget::direct_child_count ||
                term.target == MatchTarget::child_file_count ||
                term.target == MatchTarget::child_folder_count;
        });
    constexpr auto no_child_name_slot =
        std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> child_name_term_indices;
    std::vector<std::size_t> child_name_slots(terms.size(),
                                               no_child_name_slot);
    for (std::size_t index = 0; index < terms.size(); ++index) {
        if (terms[index].target != MatchTarget::child_name) continue;
        child_name_slots[index] = child_name_term_indices.size();
        child_name_term_indices.push_back(index);
    }
    const bool query_reads_direct_children = query_reads_child_counts ||
        !child_name_term_indices.empty();

    std::shared_lock lock(mutex_);
    std::vector<Candidate> candidates;
    std::priority_queue<Candidate, std::vector<Candidate>, MinScoreFirst>
        relevance_best;
    std::vector<Candidate> sorted_best;

    struct CandidateSortView {
        std::wstring_view name;
        std::wstring_view path;
        std::wstring path_storage;
        bool owns_path{};
        [[nodiscard]] std::wstring_view path_value() const {
            return owns_path ? std::wstring_view(path_storage) : path;
        }
        std::uint64_t size{};
        std::int64_t last_write_time{};
        std::int64_t creation_time{};
        std::int64_t last_access_time{};
        std::int64_t change_time{};
        std::uint32_t attributes{};
        bool directory{};
    };
    const auto candidate_view = [&](const Candidate& candidate) {
        CandidateSortView view;
        if (candidate.overlay) {
            const auto found = overlay_.find(candidate.id);
            if (found == overlay_.end()) return view;
            const auto& record = found->second;
            view.name = record.name;
            view.path = record.path;
            view.size = record.size;
            view.last_write_time = record.last_write_time;
            view.creation_time = record.creation_time;
            view.last_access_time = record.last_access_time;
            view.change_time = record.change_time;
            view.attributes = record.attributes;
            view.directory = record.directory;
            return view;
        }
        const auto& record = records_[candidate.index];
        view.name = name_view(record);
        view.owns_path = record.name_only_path;
        view.path = path_view(record, view.path_storage);
        view.size = record.size;
        view.last_write_time = record.last_write_time;
        view.attributes = record.attributes;
        view.directory = record.directory();
        return view;
    };
    const auto candidate_order = [&](const Candidate& left_candidate,
                                     const Candidate& right_candidate) {
        const auto left = candidate_view(left_candidate);
        const auto right = candidate_view(right_candidate);
        int order = 0;
        switch (options.sort) {
        case SortField::relevance:
            if (left_candidate.score != right_candidate.score) {
                order = left_candidate.score > right_candidate.score ? -1 : 1;
            }
            break;
        case SortField::name:
            order = natural_compare(left.name, right.name, sensitive);
            break;
        case SortField::path:
            order = natural_compare(left.path_value(), right.path_value(), sensitive);
            break;
        case SortField::size:
            if (left.size != right.size) order = left.size < right.size ? -1 : 1;
            break;
        case SortField::last_write_time:
            if (left.last_write_time != right.last_write_time) {
                order = left.last_write_time < right.last_write_time ? -1 : 1;
            }
            break;
        case SortField::attributes:
            if (left.attributes != right.attributes) {
                order = left.attributes < right.attributes ? -1 : 1;
            }
            break;
        case SortField::extension:
        case SortField::type:
            if (left.directory != right.directory) {
                order = left.directory ? -1 : 1;
            } else {
                order = natural_compare(extension_of(left.name),
                                        extension_of(right.name), sensitive);
            }
            break;
        case SortField::creation_time:
            if (left.creation_time != right.creation_time) {
                order = left.creation_time < right.creation_time ? -1 : 1;
            }
            break;
        case SortField::last_access_time:
            if (left.last_access_time != right.last_access_time) {
                order = left.last_access_time < right.last_access_time ? -1 : 1;
            }
            break;
        case SortField::change_time:
            if (left.change_time != right.change_time) {
                order = left.change_time < right.change_time ? -1 : 1;
            }
            break;
        case SortField::run_count:
        case SortField::last_open_time:
        case SortField::file_list_name:
            break;
        }
        if (!order) order = natural_compare(left.path_value(), right.path_value(), sensitive);
        if (!order) {
            order = left_candidate.id < right_candidate.id
                ? -1 : (left_candidate.id > right_candidate.id ? 1 : 0);
        }
        return options.descending ? -order : order;
    };
    const auto candidate_before = [&](const Candidate& left,
                                      const Candidate& right) {
        return candidate_order(left, right) < 0;
    };

    NameBigramSignature required_bigram_signature{};
    struct RequiredPathSignature {
        NameBigramSignature name;
        PathTrigramSignature parent_path;
    };
    std::vector<RequiredPathSignature> required_path_signatures;
    bool use_bigram_signature = false;
    bool use_path_trigram_signature = false;
    bool use_name_trigram_postings = false;
    std::uint32_t name_posting_bucket = 0;
    std::uint32_t name_posting_count = 0;
    bool use_path_name_trigram_postings = false;
    std::uint32_t path_name_posting_bucket = 0;
    std::uint32_t path_name_posting_count = 0;

    const auto add_required_gram = [](NameBigramSignature& signature,
                                      std::uint32_t hash) {
        for (unsigned shift : {0U, 8U, 16U, 24U}) {
            const auto bit = (hash >> shift) & 0x7fU;
            signature.words[bit >> 6U] |=
                std::uint64_t{1} << (bit & 63U);
        }
    };
    const auto add_required_path_gram = [](PathTrigramSignature& signature,
                                           std::uint32_t hash) {
        const auto bit = hash & 0xffU;
        signature.words[bit >> 6U] |=
            std::uint64_t{1} << (bit & 63U);
    };
    for (const auto term_index : mandatory_term_indices(query)) {
        if (term_index >= terms.size()) continue;
        const auto& term = terms[term_index];
        if (term.regex || term.wildcard) continue;
        const bool name_only = term.target == MatchTarget::name ||
            (term.target == MatchTarget::any && !options.match_path);
        const bool searches_path = term.target == MatchTarget::path ||
            (term.target == MatchTarget::any && options.match_path);
        if (searches_path && term.value.size() >= 3) {
            const bool component_only =
                term.value.find_first_of(L"\\/:") == std::wstring::npos;
            RequiredPathSignature required_path_signature{};
            if (component_only) {
                for (std::size_t offset = 0; offset + 1 < term.value.size();
                     ++offset) {
                    add_required_gram(
                        required_path_signature.name,
                        name_bigram_key(term.value[offset],
                                        term.value[offset + 1]));
                }
            }
            for (std::size_t offset = 0; offset + 2 < term.value.size();
                 ++offset) {
                const auto hash = name_trigram_key(
                    term.value[offset], term.value[offset + 1],
                    term.value[offset + 2]);
                if (component_only) {
                    add_required_path_gram(
                        required_path_signature.parent_path, hash);
                }
                if (name_trigram_postings_.byte_offsets.size() ==
                        name_trigram_bucket_count + 1 &&
                    name_trigram_postings_.counts.size() ==
                        name_trigram_bucket_count) {
                    const auto bucket = hash & name_trigram_bucket_mask;
                    const auto count =
                        name_trigram_postings_.counts[bucket];
                    if (!use_path_name_trigram_postings ||
                        count < path_name_posting_count) {
                        use_path_name_trigram_postings = true;
                        path_name_posting_bucket = bucket;
                        path_name_posting_count = count;
                    }
                }
            }
            if (component_only) {
                required_path_signatures.push_back(required_path_signature);
            }
        } else if (name_only && term.value.size() == 2) {
            use_bigram_signature = true;
            add_required_gram(required_bigram_signature,
                              name_bigram_key(term.value[0], term.value[1]));
        } else if (name_only && term.value.size() >= 3) {
            for (std::size_t offset = 0; offset + 2 < term.value.size();
                 ++offset) {
                if (name_trigram_postings_.byte_offsets.size() !=
                        name_trigram_bucket_count + 1 ||
                    name_trigram_postings_.counts.size() !=
                        name_trigram_bucket_count) {
                    continue;
                }
                const auto bucket = name_trigram_key(
                    term.value[offset], term.value[offset + 1],
                    term.value[offset + 2]) & name_trigram_bucket_mask;
                const auto count = name_trigram_postings_.counts[bucket];
                if (!use_name_trigram_postings ||
                    count < name_posting_count) {
                    use_name_trigram_postings = true;
                    name_posting_bucket = bucket;
                    name_posting_count = count;
                }
            }
        }
    }

    use_path_trigram_signature =
        !required_path_signatures.empty() &&
        directory_signature_bits_.size() == (records_.size() + 63U) / 64U &&
        directory_signature_rank_prefix_.size() ==
            directory_signature_bits_.size() + 1U;

    const bool relevance_bounded = options.sort == SortField::relevance &&
        !options.descending && query.duplicate_mode == DuplicateMode::none;
    const bool sorted_bounded = options.sort != SortField::relevance &&
        query.duplicate_mode == DuplicateMode::none;
    const auto consider = [&](Candidate candidate) {
        if (relevance_bounded) {
            if (relevance_best.size() < options.limit) {
                relevance_best.push(candidate);
            } else if (candidate.score > relevance_best.top().score ||
                       (candidate.score == relevance_best.top().score &&
                        candidate.id < relevance_best.top().id)) {
                relevance_best.pop();
                relevance_best.push(candidate);
            }
            return;
        }
        if (sorted_bounded) {
            if (sorted_best.size() < options.limit) {
                sorted_best.push_back(candidate);
                std::push_heap(sorted_best.begin(), sorted_best.end(),
                               candidate_before);
            } else if (candidate_before(candidate, sorted_best.front())) {
                std::pop_heap(sorted_best.begin(), sorted_best.end(),
                              candidate_before);
                sorted_best.back() = candidate;
                std::push_heap(sorted_best.begin(), sorted_best.end(),
                               candidate_before);
            }
            return;
        }
        candidates.push_back(candidate);
    };
    struct DirectChildCounts {
        std::uint32_t files{};
        std::uint32_t folders{};
    };
    const auto directory_count = directory_signature_rank_prefix_.empty()
        ? std::size_t{} : static_cast<std::size_t>(
              directory_signature_rank_prefix_.back());
    std::vector<DirectChildCounts> base_child_counts;
    std::unordered_map<std::uint64_t, DirectChildCounts> overlay_child_counts;
    std::vector<std::vector<std::uint8_t>> base_child_name_matches;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>>
        overlay_child_name_matches;
    if (query_reads_direct_children) {
        if (query_reads_child_counts) {
            base_child_counts.resize(directory_count);
            overlay_child_counts.reserve(overlay_.size());
        }
        base_child_name_matches.assign(
            child_name_term_indices.size(),
            std::vector<std::uint8_t>(directory_count));
        overlay_child_name_matches.reserve(overlay_.size());

        const auto increment_count = [](DirectChildCounts& counts,
                                        bool directory) {
            auto& value = directory ? counts.folders : counts.files;
            if (value != std::numeric_limits<std::uint32_t>::max()) ++value;
        };
        const auto mark_base_child = [&](std::size_t parent_index,
                                         std::wstring_view child_name,
                                         bool child_directory) {
            const auto directory_index =
                directory_signature_index(parent_index);
            if (directory_index >= directory_count) return;
            if (query_reads_child_counts) {
                increment_count(base_child_counts[directory_index],
                                child_directory);
            }
            for (std::size_t slot = 0;
                 slot < child_name_term_indices.size(); ++slot) {
                if (base_child_name_matches[slot][directory_index]) continue;
                if (text_match(terms[child_name_term_indices[slot]], child_name,
                               sensitive, whole_word)) {
                    base_child_name_matches[slot][directory_index] = 1;
                }
            }
        };
        const auto mark_overlay_child = [&](std::uint64_t parent,
                                            std::wstring_view child_name,
                                            bool child_directory) {
            if (query_reads_child_counts) {
                increment_count(overlay_child_counts[parent],
                                child_directory);
            }
            std::vector<std::uint8_t>* matches = nullptr;
            for (std::size_t slot = 0;
                 slot < child_name_term_indices.size(); ++slot) {
                if (!text_match(terms[child_name_term_indices[slot]],
                                child_name, sensitive, whole_word)) {
                    continue;
                }
                if (matches == nullptr) {
                    matches = &overlay_child_name_matches.try_emplace(
                        parent, child_name_term_indices.size(), 0).first->second;
                }
                (*matches)[slot] = 1;
            }
        };
        const auto mark_parent_by_id = [&](std::uint64_t child_id,
                                           std::uint64_t parent,
                                           std::wstring_view child_name,
                                           bool child_directory) {
            if (parent == 0 || parent == child_id) return;
            if (const auto overlay_parent = overlay_.find(parent);
                overlay_parent != overlay_.end()) {
                if (overlay_parent->second.directory) {
                    mark_overlay_child(parent, child_name, child_directory);
                }
                return;
            }
            const auto base_parent = std::lower_bound(
                records_.begin(), records_.end(), parent,
                [](const CompactRecord& record, std::uint64_t id) {
                    return record.id < id;
                });
            if (base_parent == records_.end() || base_parent->id != parent ||
                !base_parent->directory() ||
                std::binary_search(suppressed_base_ids_.begin(),
                                   suppressed_base_ids_.end(), parent)) {
                return;
            }
            mark_base_child(static_cast<std::size_t>(
                                std::distance(records_.begin(), base_parent)),
                            child_name, child_directory);
        };
        for (std::size_t index = 0; index < records_.size(); ++index) {
            const auto& record = records_[index];
            if (std::binary_search(suppressed_base_ids_.begin(),
                                   suppressed_base_ids_.end(), record.id)) {
                continue;
            }
            const auto child_name = name_view(record);
            if (record.parent_index != missing_parent_index &&
                record.parent_index < records_.size()) {
                const auto& base_parent = records_[record.parent_index];
                if (base_parent.id == record.id) continue;
                if (const auto overlay_parent = overlay_.find(base_parent.id);
                    overlay_parent != overlay_.end()) {
                    if (overlay_parent->second.directory) {
                        mark_overlay_child(base_parent.id, child_name,
                                           record.directory());
                    }
                    continue;
                }
                if (base_parent.directory() &&
                    !std::binary_search(suppressed_base_ids_.begin(),
                                        suppressed_base_ids_.end(),
                                        base_parent.id)) {
                    mark_base_child(record.parent_index, child_name,
                                    record.directory());
                }
                continue;
            }
            mark_parent_by_id(record.id, parent_id(record), child_name,
                              record.directory());
        }
        for (const auto& [id, record] : overlay_) {
            mark_parent_by_id(id, record.parent_id, record.name,
                              record.directory);
        }
    }
    constexpr auto no_base_index = std::numeric_limits<std::size_t>::max();
    const auto accepted = [&](std::uint64_t id, std::size_t base_index,
                              bool directory, std::wstring_view name,
                              std::wstring_view path, std::uint64_t size,
                              std::int64_t modified,
                              std::uint32_t attributes) {
        if (query.directories_only.has_value() &&
            directory != *query.directories_only) {
            return false;
        }
        DirectChildCounts child_counts;
        if (query_reads_child_counts && directory) {
            if (base_index != no_base_index) {
                const auto count_index = directory_signature_index(base_index);
                if (count_index < base_child_counts.size()) {
                    child_counts = base_child_counts[count_index];
                }
            } else if (const auto found = overlay_child_counts.find(id);
                       found != overlay_child_counts.end()) {
                child_counts = found->second;
            }
        }
        const auto child_name_matches = [&](std::size_t term_index) {
            if (!directory || term_index >= child_name_slots.size()) {
                return false;
            }
            const auto slot = child_name_slots[term_index];
            if (slot == no_child_name_slot) return false;
            if (base_index != no_base_index) {
                const auto directory_index =
                    directory_signature_index(base_index);
                return slot < base_child_name_matches.size() &&
                    directory_index < base_child_name_matches[slot].size() &&
                    base_child_name_matches[slot][directory_index] != 0;
            }
            const auto found = overlay_child_name_matches.find(id);
            return found != overlay_child_name_matches.end() &&
                slot < found->second.size() && found->second[slot] != 0;
        };
        return evaluate(query, terms, name, path, size, modified, attributes,
                        directory, child_counts.files, child_counts.folders,
                        child_name_matches, options.match_path, sensitive,
                        whole_word);
    };
    const auto signature_contains = [](const auto& candidate,
                                       const auto& required) {
        for (std::size_t word = 0; word < candidate.words.size(); ++word) {
            if ((candidate.words[word] & required.words[word]) !=
                required.words[word]) {
                return false;
            }
        }
        return true;
    };
    const auto path_signature_may_match = [&](std::size_t index) {
        if (!use_path_trigram_signature) return true;
        if (index >= records_.size() ||
            index >= name_bigram_signatures_.size()) {
            return true;
        }
        const auto owner = path_signature_owner(index);
        if (owner >= path_trigram_signatures_.size()) return true;
        for (const auto& required : required_path_signatures) {
            if (!signature_contains(path_trigram_signatures_[owner],
                                    required.parent_path) &&
                !signature_contains(name_bigram_signatures_[index],
                                    required.name)) {
                return false;
            }
        }
        return true;
    };
    const auto decode_posting = [&](std::uint32_t bucket) {
        std::vector<std::uint32_t> positions;
        if (bucket >= name_trigram_postings_.counts.size() ||
            bucket + 1 >= name_trigram_postings_.byte_offsets.size()) {
            return positions;
        }
        const auto count = name_trigram_postings_.counts[bucket];
        const auto begin = name_trigram_postings_.byte_offsets[bucket];
        const auto end = name_trigram_postings_.byte_offsets[bucket + 1];
        if (begin > end ||
            end > name_trigram_postings_.encoded_positions.size()) {
            return positions;
        }
        positions.reserve(count);
        std::size_t cursor = begin;
        std::uint32_t current = 0;
        while (cursor < end && positions.size() < count) {
            std::uint32_t delta = 0;
            unsigned shift = 0;
            for (;;) {
                if (cursor >= end || shift > 28U) {
                    positions.clear();
                    return positions;
                }
                const auto byte =
                    name_trigram_postings_.encoded_positions[cursor++];
                delta |= static_cast<std::uint32_t>(byte & 0x7fU) << shift;
                if ((byte & 0x80U) == 0) break;
                shift += 7U;
            }
            current += delta;
            if (current >= natural_name_order_.size()) {
                positions.clear();
                return positions;
            }
            positions.push_back(current);
        }
        if (cursor != end || positions.size() != count) positions.clear();
        return positions;
    };

    std::vector<std::uint32_t> name_posting_positions;
    if (use_name_trigram_postings) {
        name_posting_positions = decode_posting(name_posting_bucket);
        if (name_posting_positions.size() != name_posting_count) {
            use_name_trigram_postings = false;
        }
    }
    std::vector<std::uint32_t> path_name_posting_positions;
    if (use_path_name_trigram_postings) {
        path_name_posting_positions =
            decode_posting(path_name_posting_bucket);
        if (path_name_posting_positions.size() != path_name_posting_count) {
            use_path_name_trigram_postings = false;
        }
    }

    // Exact filelist queries can use the existing compact filename prefix
    // tables without adding a permanent filename/path hash map. Every full
    // path match must first have the exact basename from that alternative, so
    // only those small name ranges need path reconstruction and evaluation.
    bool exact_filelist_scan_complete = false;
    const bool simple_exact_filelist_query = query.terms.size() == 1 &&
        query.program.size() == 1 &&
        query.program.front().opcode == QueryOpcode::term &&
        query.program.front().term_index == 0 && !terms[0].excluded &&
        terms[0].target == MatchTarget::filename_list &&
        !terms[0].regex && !terms[0].wildcard &&
        !terms[0].alternatives.empty() &&
        name_prefix_order_.size() == records_.size();
    if (simple_exact_filelist_query) {
        std::unordered_set<std::uint32_t> inspected_base;
        inspected_base.reserve(terms[0].alternatives.size() * 4);

        const auto prefix_bounds = [&](
                std::wstring_view prefix,
                const std::vector<NamePrefixRange>& ranges,
                const std::vector<NameFirstCharacterRange>& first_ranges) {
            std::pair<std::uint32_t, std::uint32_t> bounds{};
            if (prefix.size() == 1) {
                const auto key = static_cast<std::uint16_t>(
                    normalize_char(prefix.front(), false));
                const auto found = std::lower_bound(
                    first_ranges.begin(), first_ranges.end(), key,
                    [](const NameFirstCharacterRange& range,
                       std::uint16_t value) { return range.key < value; });
                if (found != first_ranges.end() && found->key == key) {
                    bounds = {found->begin, found->end};
                }
            } else if (!prefix.empty()) {
                const auto key = name_prefix_key(prefix);
                const auto found = std::lower_bound(
                    ranges.begin(), ranges.end(), key,
                    [](const NamePrefixRange& range, std::uint32_t value) {
                        return range.key < value;
                    });
                if (found != ranges.end() && found->key == key) {
                    bounds = {found->begin, found->end};
                }
            }
            return bounds;
        };

        bool all_alternatives_accelerated = true;
        for (const auto& alternative : terms[0].alternatives) {
            auto basename = std::wstring_view(alternative);
            const auto separator = basename.find_last_of(L"\\:");
            if (separator != std::wstring_view::npos) {
                basename.remove_prefix(separator + 1);
            }
            if (basename.empty()) {
                all_alternatives_accelerated = false;
                break;
            }

            auto name_term = terms[0];
            name_term.target = MatchTarget::name;
            name_term.value.assign(basename);
            name_term.alternatives.clear();

            const auto scan_range = [&](
                    const std::vector<std::uint32_t>& order,
                    const std::vector<NamePrefixRange>& ranges,
                    const std::vector<NameFirstCharacterRange>& first_ranges) {
                const auto [begin, end] =
                    prefix_bounds(basename, ranges, first_ranges);
                for (std::uint32_t position = begin; position < end; ++position) {
                    const auto index = order[position];
                    if (!inspected_base.insert(index).second) continue;
                    const auto& record = records_[index];
                    if (std::binary_search(suppressed_base_ids_.begin(),
                                           suppressed_base_ids_.end(),
                                           record.id)) {
                        continue;
                    }
                    const auto name = name_view(record);
                    if (!exact_text_match(name_term, name, sensitive)) continue;
                    std::wstring path_scratch;
                    const auto path = query_reads_path
                        ? path_view(record, path_scratch) : std::wstring_view{};
                    if (!accepted(record.id, index, record.directory(), name,
                                  path, record.size, record.last_write_time,
                                  record.attributes)) {
                        continue;
                    }
                    consider({rank_record(terms, name, path, options.match_path,
                                          sensitive),
                              record.id, index, false});
                }
            };

            scan_range(name_prefix_order_, name_prefix_ranges_,
                       name_first_character_ranges_);
            if (!options.match_diacritics) {
                scan_range(folded_name_prefix_order_,
                           folded_name_prefix_ranges_,
                           folded_name_first_character_ranges_);
            }
        }

        if (all_alternatives_accelerated) {
            for (const auto& [id, record] : overlay_) {
                if (!accepted(id, no_base_index, record.directory, record.name,
                              record.path, record.size, record.last_write_time,
                              record.attributes)) {
                    continue;
                }
                consider({rank_record(terms, record.name, record.path,
                                      options.match_path, sensitive),
                          id, 0, true});
            }
            exact_filelist_scan_complete = true;
        } else {
            relevance_best = {};
            sorted_best.clear();
            candidates.clear();
        }
    }

    // The GUI defaults to case-insensitive natural name order. Walking a
    // pre-sorted base order and merging the small delta overlay means we only
    // inspect records until the requested page is complete; the old path
    // materialized and sorted every match, which made one-character queries
    // take seconds on multi-million-file catalogs.
    if (!exact_filelist_scan_complete &&
        options.sort == SortField::name && !sensitive &&
        query.duplicate_mode == DuplicateMode::none &&
        natural_name_order_.size() == records_.size()) {
        std::vector<Candidate> overlay_matches;
        overlay_matches.reserve(std::min(overlay_.size(), options.limit));
        for (const auto& [id, record] : overlay_) {
            if (accepted(id, no_base_index, record.directory, record.name,
                         record.path,
                         record.size, record.last_write_time,
                         record.attributes)) {
                overlay_matches.push_back({
                    rank_record(terms, record.name, record.path,
                                options.match_path, sensitive),
                    id, 0, true});
            }
        }
        std::sort(overlay_matches.begin(), overlay_matches.end(),
                  candidate_before);

        const auto base_order_size = use_name_trigram_postings
            ? name_posting_positions.size()
            : natural_name_order_.size();
        std::size_t order_position = options.descending ? base_order_size : 0;
        const auto next_base_match = [&]() -> std::optional<Candidate> {
            while ((!options.descending && order_position < base_order_size) ||
                   (options.descending && order_position != 0)) {
                const auto position = options.descending
                    ? --order_position : order_position++;
                const auto natural_position = use_name_trigram_postings
                    ? name_posting_positions[position]
                    : static_cast<std::uint32_t>(position);
                const auto index = natural_name_order_[natural_position];
                const auto& record = records_[index];
                if (!suppressed_base_ids_.empty() &&
                    std::binary_search(suppressed_base_ids_.begin(),
                                       suppressed_base_ids_.end(), record.id)) {
                    continue;
                }
                if (use_bigram_signature &&
                    !signature_contains(name_bigram_signatures_[index],
                                        required_bigram_signature)) {
                    continue;
                }
                if (!path_signature_may_match(index)) continue;
                const auto name = name_view(record);
                std::wstring path_scratch;
                const auto path = query_reads_path
                    ? path_view(record, path_scratch) : std::wstring_view{};
                if (!accepted(record.id, index, record.directory(), name, path,
                              record.size,
                              record.last_write_time, record.attributes)) {
                    continue;
                }
                return Candidate{
                    rank_record(terms, name, path, options.match_path, sensitive),
                    record.id, index, false};
            }
            return std::nullopt;
        };

        std::vector<Candidate> ordered;
        ordered.reserve(options.limit);
        std::size_t overlay_position = 0;
        auto base_match = next_base_match();
        while (ordered.size() < options.limit &&
               (base_match.has_value() ||
                overlay_position < overlay_matches.size())) {
            if (!base_match.has_value()) {
                ordered.push_back(overlay_matches[overlay_position++]);
                continue;
            }
            if (overlay_position >= overlay_matches.size()) {
                ordered.push_back(*base_match);
                base_match = next_base_match();
                continue;
            }
            if (candidate_before(overlay_matches[overlay_position],
                                 *base_match)) {
                ordered.push_back(overlay_matches[overlay_position++]);
            } else {
                ordered.push_back(*base_match);
                base_match = next_base_match();
            }
        }

        std::vector<SearchResult> results;
        results.reserve(ordered.size());
        for (const auto& candidate : ordered) {
            if (candidate.overlay) {
                const auto found = overlay_.find(candidate.id);
                if (found != overlay_.end()) {
                    results.push_back({found->second, candidate.score});
                }
            } else {
                results.push_back(
                    {materialize(records_[candidate.index]), candidate.score});
            }
        }
        return results;
    }

    // A plain one-term relevance query is the GUI's hot path. Exact and
    // prefix matches always outrank substring-only matches, so if the prefix
    // range already fills the requested page, the rest of the catalog cannot
    // affect the result set.
    bool prefix_result_complete = exact_filelist_scan_complete;
    const bool simple_prefix_query = relevance_bounded && query.terms.size() == 1 &&
        query.program.size() == 1 &&
        query.program.front().opcode == QueryOpcode::term &&
        query.program.front().term_index == 0 && !terms[0].excluded &&
        !terms[0].regex && !terms[0].wildcard && !terms[0].value.empty() &&
        (terms[0].target == MatchTarget::name ||
         (terms[0].target == MatchTarget::any && !options.match_path));
    const bool simple_path_query = relevance_bounded && query.terms.size() == 1 &&
        query.program.size() == 1 &&
        query.program.front().opcode == QueryOpcode::term &&
        query.program.front().term_index == 0 && !terms[0].excluded &&
        !terms[0].regex && !terms[0].wildcard &&
        (terms[0].target == MatchTarget::path ||
         (terms[0].target == MatchTarget::any && options.match_path)) &&
        terms[0].value.size() >= 3;
    if (simple_prefix_query) {
        const auto& prefix_term = terms[0];
        const auto prefix_bounds = [&prefix_term](
                const std::vector<NamePrefixRange>& ranges,
                const std::vector<NameFirstCharacterRange>& first_ranges) {
            std::pair<std::uint32_t, std::uint32_t> bounds{};
            if (prefix_term.value.size() == 1) {
                const auto key = static_cast<std::uint16_t>(
                    normalize_char(prefix_term.value[0], false));
                const auto found = std::lower_bound(
                    first_ranges.begin(), first_ranges.end(), key,
                    [](const NameFirstCharacterRange& range,
                       std::uint16_t value) { return range.key < value; });
                if (found != first_ranges.end() && found->key == key) {
                    bounds = {found->begin, found->end};
                }
            } else {
                const auto key = name_prefix_key(prefix_term.value);
                const auto found = std::lower_bound(
                    ranges.begin(), ranges.end(), key,
                    [](const NamePrefixRange& range, std::uint32_t value) {
                        return range.key < value;
                    });
                if (found != ranges.end() && found->key == key) {
                    bounds = {found->begin, found->end};
                }
            }
            return bounds;
        };
        const auto in_raw_prefix_range = [&prefix_term](
                std::wstring_view name) {
            if (name.empty()) return false;
            if (prefix_term.value.size() == 1) {
                return normalize_char(name.front(), false) ==
                    normalize_char(prefix_term.value.front(), false);
            }
            return name_prefix_key(name) == name_prefix_key(prefix_term.value);
        };

        std::size_t accepted_prefixes = 0;
        const auto scan_prefix_order = [&](
                const std::vector<std::uint32_t>& order,
                const std::vector<NamePrefixRange>& ranges,
                const std::vector<NameFirstCharacterRange>& first_ranges,
                bool folded_only) {
            const auto [prefix_begin, prefix_end] =
                prefix_bounds(ranges, first_ranges);
            for (std::uint32_t position = prefix_begin;
                 position < prefix_end; ++position) {
                const auto index = order[position];
                const auto& record = records_[index];
                if (std::binary_search(suppressed_base_ids_.begin(),
                                       suppressed_base_ids_.end(), record.id)) {
                    continue;
                }
                const auto name = name_view(record);
                // A folded entry can share the ordinary raw prefix range
                // (for example cafe/café). The raw pass already evaluated it.
                if (folded_only && in_raw_prefix_range(name)) continue;
                if (!prefix_text_match(prefix_term, name, sensitive)) continue;
                std::wstring path_scratch;
                const auto path = query_reads_path
                    ? path_view(record, path_scratch) : std::wstring_view{};
                if (!accepted(record.id, index, record.directory(), name, path,
                              record.size,
                              record.last_write_time, record.attributes)) {
                    continue;
                }
                ++accepted_prefixes;
                consider({rank_record(terms, name, path, options.match_path,
                                      sensitive),
                          record.id, index, false});
            }
        };
        scan_prefix_order(name_prefix_order_, name_prefix_ranges_,
                          name_first_character_ranges_, false);
        if (!options.match_diacritics) {
            scan_prefix_order(folded_name_prefix_order_,
                              folded_name_prefix_ranges_,
                              folded_name_first_character_ranges_, true);
        }
        for (const auto& [id, record] : overlay_) {
            if (!prefix_text_match(prefix_term, record.name, sensitive) ||
                !accepted(id, no_base_index, record.directory, record.name,
                         record.path,
                          record.size, record.last_write_time,
                          record.attributes)) {
                continue;
            }
            ++accepted_prefixes;
            consider({rank_record(terms, record.name, record.path,
                                  options.match_path, sensitive),
                      id, 0, true});
        }
        prefix_result_complete = accepted_prefixes >= options.limit;
    }

    // For a simple path query, filename matches are the only candidates that
    // can score above a path-only match. Resolve that small, contiguous Bloom
    // scan first. The subsequent path scan can then stop as soon as the first
    // page of path-only matches is stable instead of traversing the catalog.
    if (simple_path_query) {
        const auto& path_term = terms[0];
        const auto candidate_count = use_path_name_trigram_postings
            ? path_name_posting_positions.size()
            : records_.size();
        for (std::size_t position = 0; position < candidate_count; ++position) {
            const auto index = use_path_name_trigram_postings
                ? natural_name_order_[path_name_posting_positions[position]]
                : position;
            const auto& record = records_[index];
            if (std::binary_search(suppressed_base_ids_.begin(),
                                   suppressed_base_ids_.end(), record.id)) {
                continue;
            }
            const auto name = name_view(record);
            if (!text_match(path_term, name, sensitive, whole_word)) continue;
            std::wstring path_scratch;
            const auto path = query_reads_path
                    ? path_view(record, path_scratch) : std::wstring_view{};
            if (!accepted(record.id, index, record.directory(), name, path,
                              record.size,
                          record.last_write_time, record.attributes)) {
                continue;
            }
            consider({rank_record(terms, name, path, options.match_path,
                                  sensitive),
                      record.id, index, false});
        }
        for (const auto& [id, record] : overlay_) {
            if (!text_match(path_term, record.name, sensitive, whole_word) ||
                !accepted(id, no_base_index, record.directory, record.name,
                         record.path,
                          record.size, record.last_write_time,
                          record.attributes)) {
                continue;
            }
            consider({rank_record(terms, record.name, record.path,
                                  options.match_path, sensitive),
                      id, 0, true});
        }
        prefix_result_complete = relevance_best.size() >= options.limit &&
            relevance_best.top().score >= 50;
    }

    if (!prefix_result_complete) {
        // Overlay IDs are not ordered, so account for all of them before the
        // ordered base scan. Prefix overlay records were already considered.
        if (simple_prefix_query || simple_path_query) {
            for (const auto& [id, record] : overlay_) {
                const bool already_considered = simple_prefix_query
                    ? prefix_text_match(terms[0], record.name, sensitive)
                    : text_match(terms[0], record.name, sensitive, whole_word);
                if (already_considered) continue;
                if (accepted(id, no_base_index, record.directory, record.name,
                         record.path,
                             record.size, record.last_write_time,
                             record.attributes)) {
                    consider({rank_record(terms, record.name, record.path,
                                          options.match_path, sensitive),
                              id, 0, true});
                }
            }
        }

        std::size_t suppressed = 0;
        const auto inspect_base = [&](std::size_t index) {
            const auto& record = records_[index];
            if (use_name_trigram_postings) {
                if (std::binary_search(suppressed_base_ids_.begin(),
                                       suppressed_base_ids_.end(), record.id)) {
                    return;
                }
            } else {
                while (suppressed < suppressed_base_ids_.size() &&
                       suppressed_base_ids_[suppressed] < record.id) {
                    ++suppressed;
                }
                if (suppressed < suppressed_base_ids_.size() &&
                    suppressed_base_ids_[suppressed] == record.id) {
                    return;
                }
            }
            const auto name = name_view(record);
            if ((simple_prefix_query &&
                 prefix_text_match(terms[0], name, sensitive)) ||
                (simple_path_query &&
                 text_match(terms[0], name, sensitive, whole_word))) {
                return;
            }
            std::wstring path_scratch;
            const auto path = query_reads_path
                    ? path_view(record, path_scratch) : std::wstring_view{};
            if (accepted(record.id, index, record.directory(), name, path,
                              record.size,
                         record.last_write_time, record.attributes)) {
                consider({rank_record(terms, name, path, options.match_path,
                                      sensitive),
                          record.id, index, false});
            }
        };
        const auto remaining_base_cannot_improve = [&](std::size_t index) {
            if (relevance_best.size() < options.limit ||
                records_[index].id <= relevance_best.top().id) {
                return false;
            }
            if (simple_prefix_query) return relevance_best.top().score >= 50;
            if (simple_path_query) return relevance_best.top().score >= 20;
            return false;
        };

        if (use_name_trigram_postings) {
            for (const auto natural_position : name_posting_positions) {
                const auto index = natural_name_order_[natural_position];
                if (use_bigram_signature &&
                    !signature_contains(name_bigram_signatures_[index],
                                        required_bigram_signature)) {
                    continue;
                }
                if (!path_signature_may_match(index)) continue;
                inspect_base(index);
            }
        } else if (use_bigram_signature || use_path_trigram_signature) {
            for (std::size_t index = 0; index < records_.size(); ++index) {
                if (remaining_base_cannot_improve(index)) break;
                if (use_bigram_signature &&
                    !signature_contains(name_bigram_signatures_[index],
                                        required_bigram_signature)) {
                    continue;
                }
                if (!path_signature_may_match(index)) continue;
                inspect_base(index);
            }
        } else {
            for (std::size_t index = 0; index < records_.size(); ++index) {
                if (remaining_base_cannot_improve(index)) break;
                inspect_base(index);
            }
        }
        if (!simple_prefix_query && !simple_path_query) {
            for (const auto& [id, record] : overlay_) {
                if (accepted(id, no_base_index, record.directory, record.name,
                         record.path,
                             record.size, record.last_write_time,
                             record.attributes)) {
                    consider({rank_record(terms, record.name, record.path,
                                          options.match_path, sensitive),
                              id, 0, true});
                }
            }
        }
    }

    if (relevance_bounded) {
        candidates.reserve(relevance_best.size());
        while (!relevance_best.empty()) {
            candidates.push_back(relevance_best.top());
            relevance_best.pop();
        }
    } else if (sorted_bounded) {
        candidates = std::move(sorted_best);
    }
    std::vector<SearchResult> results;
    results.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (candidate.overlay) {
            const auto found = overlay_.find(candidate.id);
            if (found != overlay_.end()) {
                results.push_back({found->second, candidate.score});
            }
        } else {
            results.push_back(
                {materialize(records_[candidate.index]), candidate.score});
        }
    }

    if (query.duplicate_mode != DuplicateMode::none) {
        std::unordered_map<std::wstring, std::size_t> counts;
        counts.reserve(results.size());
        const auto key = [&](const FileRecord& record) {
            if (query.duplicate_mode == DuplicateMode::size) {
                return std::to_wstring(record.size);
            }
            std::wstring value = fold_case(record.name);
            if (query.duplicate_mode == DuplicateMode::name_and_size) {
                value.push_back(L'\0');
                value += std::to_wstring(record.size);
            }
            return value;
        };
        for (const auto& result : results) ++counts[key(result.record)];
        results.erase(
            std::remove_if(results.begin(), results.end(),
                           [&](const SearchResult& result) {
                               return counts[key(result.record)] < 2;
                           }),
            results.end());
    }

    const auto compare = [&](const SearchResult& a, const SearchResult& b) {
        int order = 0;
        switch (options.sort) {
        case SortField::relevance:
            if (a.score != b.score) order = a.score > b.score ? -1 : 1;
            break;
        case SortField::name:
            order = natural_compare(a.record.name, b.record.name, sensitive);
            break;
        case SortField::path:
            order = natural_compare(a.record.path, b.record.path, sensitive);
            break;
        case SortField::size:
            if (a.record.size != b.record.size) {
                order = a.record.size < b.record.size ? -1 : 1;
            }
            break;
        case SortField::last_write_time:
            if (a.record.last_write_time != b.record.last_write_time) {
                order = a.record.last_write_time < b.record.last_write_time
                    ? -1 : 1;
            }
            break;
        case SortField::attributes:
            if (a.record.attributes != b.record.attributes) {
                order = a.record.attributes < b.record.attributes ? -1 : 1;
            }
            break;
        case SortField::extension:
        case SortField::type:
            if (a.record.directory != b.record.directory) {
                order = a.record.directory ? -1 : 1;
            } else {
                order = natural_compare(extension_of(a.record.name),
                                        extension_of(b.record.name), sensitive);
            }
            break;
        case SortField::creation_time:
            if (a.record.creation_time != b.record.creation_time) {
                order = a.record.creation_time < b.record.creation_time ? -1 : 1;
            }
            break;
        case SortField::last_access_time:
            if (a.record.last_access_time != b.record.last_access_time) {
                order = a.record.last_access_time < b.record.last_access_time ? -1 : 1;
            }
            break;
        case SortField::change_time:
            if (a.record.change_time != b.record.change_time) {
                order = a.record.change_time < b.record.change_time ? -1 : 1;
            }
            break;
        case SortField::run_count:
        case SortField::last_open_time:
        case SortField::file_list_name:
            // These fields are owned by the GUI session/user profile and are
            // sorted after the service response is received.
            break;
        }
        if (!order) {
            order = natural_compare(a.record.path, b.record.path, sensitive);
        }
        if (!order) {
            order = a.record.id < b.record.id
                ? -1 : (a.record.id > b.record.id ? 1 : 0);
        }
        return options.descending ? order > 0 : order < 0;
    };
    std::sort(results.begin(), results.end(), compare);
    if (results.size() > options.limit) results.resize(options.limit);
    return results;
}

std::size_t MetadataIndex::size() const {
    std::shared_lock lock(mutex_);
    return live_size_;
}
std::size_t MetadataIndex::pending_delta_size() const {
    std::shared_lock lock(mutex_);
    return overlay_.size() + removed_.size();
}
}

#include "esm/index.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <regex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <windows.h>
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
    MatchTarget target{MatchTarget::any}; std::wstring value,wildcard_pattern;
    bool excluded{},wildcard{},regex{},valid{true},match_diacritics{true}; NumericComparison comparison{NumericComparison::equal};
    std::uint64_t numeric_value{}; std::uint32_t attribute_mask{}; bool attribute_absent{};
    std::optional<std::wregex> regex_pattern; std::array<std::uint32_t,256> skip{};
};
CompiledTerm compile_term(const QueryTerm& term,bool sensitive,bool match_diacritics){CompiledTerm r;r.target=term.target;r.match_diacritics=match_diacritics;r.value=normalize_match_text(term.value,sensitive,match_diacritics);r.excluded=term.excluded;r.wildcard=term.wildcard;r.regex=term.regex;r.comparison=term.comparison;r.numeric_value=term.numeric_value;r.attribute_mask=term.attribute_mask;r.attribute_absent=term.attribute_absent;r.skip.fill((std::uint32_t)std::max<std::size_t>(1,r.value.size()));if(r.value.size()>1)for(std::size_t i=0;i+1<r.value.size();++i)if(r.value[i]<256)r.skip[(unsigned)r.value[i]]=(std::uint32_t)(r.value.size()-i-1);if(r.wildcard){r.wildcard_pattern=L"*"+r.value+L"*";}if(r.regex){try{auto flags=std::regex_constants::ECMAScript;const auto pattern=normalize_match_text(term.value,sensitive,match_diacritics);r.regex_pattern.emplace(pattern,flags);}catch(const std::regex_error&){r.valid=false;}}return r;}
bool starts_text(std::wstring_view text,const CompiledTerm& term,bool sensitive){if(text.size()<term.value.size())return false;for(std::size_t i=0;i<term.value.size();++i)if(normalize_char(text[i],sensitive)!=term.value[i])return false;return true;}
bool equal_text(std::wstring_view text,const CompiledTerm& term,bool sensitive){return text.size()==term.value.size()&&starts_text(text,term,sensitive);}
bool contains_text(std::wstring_view text,const CompiledTerm& term,bool sensitive,bool whole_word){auto needle=std::wstring_view(term.value);if(needle.empty())return true;if(needle.size()>text.size())return false;std::size_t offset=0;while(offset+needle.size()<=text.size()){std::size_t j=needle.size();while(j&&normalize_char(text[offset+j-1],sensitive)==needle[j-1])--j;if(!j){bool left=offset==0||!word_char(text[offset-1]);bool right=offset+needle.size()==text.size()||!word_char(text[offset+needle.size()]);if(!whole_word||(left&&right))return true;++offset;continue;}wchar_t tail=normalize_char(text[offset+needle.size()-1],sensitive);std::size_t shift=needle.size();if(tail<256)shift=term.skip[(unsigned)tail];else for(std::size_t i=0;i+1<needle.size();++i)if(needle[i]==tail)shift=needle.size()-i-1;offset+=std::max<std::size_t>(1,shift);}return false;}
bool wildcard_text(const CompiledTerm& term,std::wstring_view value,bool sensitive){auto ptn=std::wstring_view(term.wildcard_pattern);std::size_t p=0,v=0,star=std::wstring_view::npos,checkpoint=0;while(v<value.size()){bool same=p<ptn.size()&&ptn[p]!=L'*'&&ptn[p]!=L'?'&&normalize_char(value[v],sensitive)==ptn[p];if(p<ptn.size()&&(ptn[p]==L'?'||same)){++p;++v;}else if(p<ptn.size()&&ptn[p]==L'*'){star=p++;checkpoint=v;}else if(star!=std::wstring_view::npos){p=star+1;v=++checkpoint;}else return false;}while(p<ptn.size()&&ptn[p]==L'*')++p;return p==ptn.size();}
std::wstring_view extension_of(std::wstring_view name){auto dot=name.find_last_of(L'.');return dot==std::wstring_view::npos||dot+1==name.size()?std::wstring_view{}:name.substr(dot+1);}
bool compare_number(std::uint64_t value,const CompiledTerm& term){switch(term.comparison){case NumericComparison::equal:return value==term.numeric_value;case NumericComparison::less:return value<term.numeric_value;case NumericComparison::less_equal:return value<=term.numeric_value;case NumericComparison::greater:return value>term.numeric_value;case NumericComparison::greater_equal:return value>=term.numeric_value;}return false;}
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
bool term_matches(const CompiledTerm& term,std::wstring_view name,std::wstring_view path,std::uint64_t size,std::int64_t modified,std::uint32_t attributes,bool match_path,bool sensitive,bool whole_word){switch(term.target){case MatchTarget::name:return text_match(term,name,sensitive,whole_word);case MatchTarget::path:return text_match(term,path,sensitive,whole_word);case MatchTarget::extension:{auto ext=extension_of(name);return term.regex||term.wildcard?text_match(term,ext,sensitive,whole_word):exact_text_match(term,ext,sensitive);}case MatchTarget::size:return compare_number(size,term);case MatchTarget::last_write_time:return compare_number(modified<0?0:(std::uint64_t)modified,term);case MatchTarget::attributes:{bool present=(attributes&term.attribute_mask)==term.attribute_mask;return term.attribute_absent?!present:present;}case MatchTarget::any:return text_match(term,name,sensitive,whole_word)||(match_path&&text_match(term,path,sensitive,whole_word));}return false;}
bool evaluate(const ParsedQuery& query,const std::vector<CompiledTerm>& terms,std::wstring_view name,std::wstring_view path,std::uint64_t size,std::int64_t modified,std::uint32_t attributes,bool match_path,bool sensitive,bool whole_word){if(query.program.empty())return true;std::vector<bool> stack;stack.reserve(query.terms.size());for(auto& instruction:query.program){if(instruction.opcode==QueryOpcode::term){if(instruction.term_index>=terms.size())return false;stack.push_back(term_matches(terms[instruction.term_index],name,path,size,modified,attributes,match_path,sensitive,whole_word));}else if(instruction.opcode==QueryOpcode::logical_not){if(stack.empty())return false;stack.back()=!stack.back();}else{if(stack.size()<2)return false;bool right=stack.back();stack.pop_back();bool left=stack.back();stack.back()=instruction.opcode==QueryOpcode::logical_and?(left&&right):(left||right);}}return stack.size()==1&&stack.back();}
int rank_record(const std::vector<CompiledTerm>& terms,
                std::wstring_view name, std::wstring_view path,
                bool match_path, bool sensitive) {
    int score = 0;
    for (const auto& term : terms) {
        if (term.excluded || term.target == MatchTarget::size ||
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

    NameSearchAccelerators accelerators;
    accelerators.bigram_signatures.resize(records.size());
    accelerators.path_trigram_signatures.resize(records.size());
    std::vector<std::uint64_t> prefix_entries;
    prefix_entries.reserve(records.size());
    std::vector<std::uint64_t> folded_prefix_entries;

    const auto add_gram = [](NameGramSignature& signature,
                             std::uint32_t hash) {
        for (unsigned shift : {0U, 8U, 16U, 24U}) {
            const auto bit = (hash >> shift) & 0xffU;
            signature.words[bit >> 6U] |=
                std::uint64_t{1} << (bit & 63U);
        }
    };

    for (std::size_t record_index = 0; record_index < records.size();
         ++record_index) {
        const auto& record = records[record_index];
        const std::wstring_view name(strings.data() + record.name_offset,
                                     record.name_length);
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

        const std::wstring_view path(strings.data() + record.path_offset,
                                     record.path_length);
        const auto add_path_trigrams = [&](std::wstring_view value) {
            auto& signature =
                accelerators.path_trigram_signatures[record_index];
            for (std::size_t offset = 0; offset + 2 < value.size(); ++offset) {
                // Full paths contain many more grams than file names. Four
                // Bloom bits per gram saturates a 256-bit signature and lets
                // too many false positives through. One well-distributed bit
                // is substantially more selective for typical 50-150 character
                // paths while retaining the no-false-negative property.
                const auto bit = name_trigram_key(
                    value[offset], value[offset + 1], value[offset + 2]) &
                    0xffU;
                signature.words[bit >> 6U] |=
                    std::uint64_t{1} << (bit & 63U);
            }
        };
        add_path_trigrams(path);

        // Store accent-folded grams in the same Bloom signatures. This keeps
        // diacritic-insensitive searches on the indexed path without adding a
        // second per-record signature array. The raw grams remain present, so
        // diacritic-sensitive searches retain their exact semantics.
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
        if (std::any_of(path.begin(), path.end(),
                        [](wchar_t ch) { return ch >= 0x80; })) {
            const auto folded = normalize_match_text(path, false, false);
            bool differs = folded.size() != path.size();
            if (!differs) {
                for (std::size_t offset = 0; offset < path.size(); ++offset) {
                    if (folded[offset] != normalize_char(path[offset], false)) {
                        differs = true;
                        break;
                    }
                }
            }
            if (differs) add_path_trigrams(folded);
        }
    }

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
            std::wstring_view(strings.data() + record.name_offset,
                              record.name_length));
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
                strings.data() + left.name_offset, left.name_length);
            const std::wstring_view right_name(
                strings.data() + right.name_offset, right.name_length);
            int order = natural_compare(left_name, right_name, false);
            if (!order) {
                const std::wstring_view left_path(
                    strings.data() + left.path_offset, left.path_length);
                const std::wstring_view right_path(
                    strings.data() + right.path_offset, right.path_length);
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
    // touch every record. Build a compact CSR posting table instead. Each
    // 16-bit trigram hash points at record indices already arranged in natural
    // name order, so default GUI searches inspect only a narrow candidate list
    // and can stop as soon as a page is full. Hash collisions are harmless
    // because the complete query evaluator always verifies every candidate.
    std::vector<std::uint32_t> posting_counts(name_trigram_bucket_count);
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
            const std::wstring_view name(strings.data() + record.name_offset,
                                         record.name_length);
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

    for (const auto record_index : accelerators.natural_name_order) {
        visit_record_trigram_buckets(record_index, [&](std::uint32_t bucket) {
            if (posting_counts[bucket] ==
                std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("name trigram posting bucket overflow");
            }
            ++posting_counts[bucket];
        });
    }
    accelerators.trigram_postings.offsets.resize(
        name_trigram_bucket_count + 1);
    std::uint64_t posting_count = 0;
    for (std::size_t bucket = 0; bucket < name_trigram_bucket_count; ++bucket) {
        accelerators.trigram_postings.offsets[bucket] =
            static_cast<std::uint32_t>(posting_count);
        posting_count += posting_counts[bucket];
        if (posting_count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error(
                "name trigram posting index exceeds 32-bit offsets");
        }
    }
    accelerators.trigram_postings.offsets.back() =
        static_cast<std::uint32_t>(posting_count);
    accelerators.trigram_postings.record_indices.resize(
        static_cast<std::size_t>(posting_count));
    auto cursors = accelerators.trigram_postings.offsets;
    std::fill(seen.begin(), seen.end(), 0);
    generation = 0;
    for (const auto record_index : accelerators.natural_name_order) {
        visit_record_trigram_buckets(record_index, [&](std::uint32_t bucket) {
            accelerators.trigram_postings.record_indices[cursors[bucket]++] =
                record_index;
        });
    }
    return accelerators;
}

void MetadataIndex::replace(const std::vector<FileRecord>& source) {
    std::size_t total_chars = 0;
    for (const auto& item : source) total_chars += item.path.size();
    if (total_chars > std::numeric_limits<std::uint32_t>::max()) throw std::length_error("string arena exceeds 32-bit offsets");
    std::vector<CompactRecord> records;
    std::vector<wchar_t> strings;
    records.reserve(source.size()); strings.reserve(total_chars);
    for (const auto& item : source) {
        CompactRecord record;
        record.id = item.id; record.parent_id = item.parent_id; record.size = item.size; record.last_write_time = item.last_write_time;
        record.attributes = item.attributes; record.directory = item.directory;
        record.path_offset = static_cast<std::uint32_t>(strings.size()); record.path_length = static_cast<std::uint32_t>(item.path.size());
        strings.insert(strings.end(), item.path.begin(), item.path.end());
        std::size_t relative_name = item.path.size();
        if (!item.name.empty() && item.path.size() >= item.name.size() && item.path.ends_with(item.name)) relative_name = item.path.size() - item.name.size();
        else { const auto slash = item.path.find_last_of(L"\\/"); relative_name = slash == std::wstring::npos ? 0 : slash + 1; }
        record.name_offset = record.path_offset + static_cast<std::uint32_t>(relative_name);
        record.name_length = record.path_length - static_cast<std::uint32_t>(relative_name);
        records.push_back(record);
    }
    std::sort(records.begin(), records.end(),
              [](const CompactRecord& a, const CompactRecord& b) {
                  return a.id < b.id;
              });
    auto name_accelerators = build_name_search_accelerators(records, strings);
    std::unordered_map<std::uint64_t, FileRecord> old_overlay;
    std::unordered_set<std::uint64_t> old_removed;
    std::vector<std::uint64_t> old_suppressed_base_ids;
    {
        std::unique_lock lock(mutex_);
        // Swap in the fully-built index while holding the lock, then destroy
        // the previous multi-million-record buffers after readers can resume.
        records_.swap(records);
        strings_.swap(strings);
        name_bigram_signatures_.swap(name_accelerators.bigram_signatures);
        std::swap(name_trigram_postings_, name_accelerators.trigram_postings);
        path_trigram_signatures_.swap(
            name_accelerators.path_trigram_signatures);
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
    for (const auto& record : records_) {
        if (removed_.find(record.id) == removed_.end() &&
            overlay_.find(record.id) == overlay_.end()) {
            total_chars += record.path_length;
        }
    }
    for (const auto& [id, record] : overlay_) {
        (void)id;
        total_chars += record.path.size();
    }
    if (total_chars > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("string arena exceeds 32-bit offsets");
    }

    std::vector<CompactRecord> compacted;
    std::vector<wchar_t> strings;
    compacted.reserve(live_size_);
    strings.reserve(total_chars);

    for (const auto& source : records_) {
        if (removed_.find(source.id) != removed_.end() ||
            overlay_.find(source.id) != overlay_.end()) {
            continue;
        }
        CompactRecord record = source;
        const auto relative_name = source.name_offset - source.path_offset;
        record.path_offset = static_cast<std::uint32_t>(strings.size());
        record.name_offset = record.path_offset + relative_name;
        const auto path = path_view(source);
        strings.insert(strings.end(), path.begin(), path.end());
        compacted.push_back(record);
    }

    for (const auto& [id, source] : overlay_) {
        (void)id;
        CompactRecord record;
        record.id = source.id;
        record.parent_id = source.parent_id;
        record.size = source.size;
        record.last_write_time = source.last_write_time;
        record.attributes = source.attributes;
        record.directory = source.directory;
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
        record.name_offset = record.path_offset +
                             static_cast<std::uint32_t>(relative_name);
        record.name_length = record.path_length -
                             static_cast<std::uint32_t>(relative_name);
        compacted.push_back(record);
    }

    std::sort(compacted.begin(), compacted.end(),
              [](const CompactRecord& a, const CompactRecord& b) {
                  return a.id < b.id;
              });
    auto name_accelerators = build_name_search_accelerators(compacted, strings);
    records_ = std::move(compacted);
    strings_ = std::move(strings);
    name_bigram_signatures_ = std::move(name_accelerators.bigram_signatures);
    name_trigram_postings_ = std::move(name_accelerators.trigram_postings);
    path_trigram_signatures_ =
        std::move(name_accelerators.path_trigram_signatures);
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

std::wstring_view MetadataIndex::path_view(const CompactRecord& record) const { return {strings_.data() + record.path_offset, record.path_length}; }
std::wstring_view MetadataIndex::name_view(const CompactRecord& record) const { return {strings_.data() + record.name_offset, record.name_length}; }
FileRecord MetadataIndex::materialize(const CompactRecord& item) const {
    FileRecord result;
    result.id = item.id; result.parent_id = item.parent_id; result.size = item.size; result.last_write_time = item.last_write_time;
    result.attributes = item.attributes; result.directory = item.directory; result.path.assign(path_view(item)); result.name.assign(name_view(item));
    return result;
}
std::vector<SearchResult> MetadataIndex::search(
    std::wstring_view text, const SearchOptions& options) const {
    if (options.limit == 0) return {};
    const ParsedQuery query = parse_query(text);
    if (!query.valid) return {};
    if (query.terms.empty() && !query.directories_only.has_value()) return {};

    const bool sensitive = query.case_sensitive.value_or(options.case_sensitive);
    const bool whole_word = query.whole_word.value_or(options.whole_word);
    std::vector<CompiledTerm> terms;
    terms.reserve(query.terms.size());
    for (const auto& term : query.terms) {
        terms.push_back(compile_term(term, sensitive, options.match_diacritics));
        if (!terms.back().valid) return {};
    }

    std::shared_lock lock(mutex_);
    std::vector<Candidate> candidates;
    std::priority_queue<Candidate, std::vector<Candidate>, MinScoreFirst>
        relevance_best;
    std::vector<Candidate> sorted_best;

    struct CandidateSortView {
        std::wstring_view name;
        std::wstring_view path;
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
        view.path = path_view(record);
        view.size = record.size;
        view.last_write_time = record.last_write_time;
        view.attributes = record.attributes;
        view.directory = record.directory;
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
            order = natural_compare(left.path, right.path, sensitive);
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
        if (!order) order = natural_compare(left.path, right.path, sensitive);
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

    NameGramSignature required_bigram_signature{};
    NameGramSignature required_path_trigram_signature{};
    bool use_bigram_signature = false;
    bool use_path_trigram_signature = false;
    bool use_name_trigram_postings = false;
    std::uint32_t name_posting_begin = 0;
    std::uint32_t name_posting_end = 0;
    bool use_path_name_trigram_postings = false;
    std::uint32_t path_name_posting_begin = 0;
    std::uint32_t path_name_posting_end = 0;

    const auto add_required_gram = [](NameGramSignature& signature,
                                      std::uint32_t hash) {
        for (unsigned shift : {0U, 8U, 16U, 24U}) {
            const auto bit = (hash >> shift) & 0xffU;
            signature.words[bit >> 6U] |=
                std::uint64_t{1} << (bit & 63U);
        }
    };
    const auto add_required_path_gram = [](NameGramSignature& signature,
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
            use_path_trigram_signature = true;
            for (std::size_t offset = 0; offset + 2 < term.value.size();
                 ++offset) {
                const auto hash = name_trigram_key(
                    term.value[offset], term.value[offset + 1],
                    term.value[offset + 2]);
                add_required_path_gram(required_path_trigram_signature, hash);
                if (name_trigram_postings_.offsets.size() ==
                    name_trigram_bucket_count + 1) {
                    const auto bucket = hash & name_trigram_bucket_mask;
                    const auto begin = name_trigram_postings_.offsets[bucket];
                    const auto end =
                        name_trigram_postings_.offsets[bucket + 1];
                    if (!use_path_name_trigram_postings ||
                        end - begin <
                            path_name_posting_end - path_name_posting_begin) {
                        use_path_name_trigram_postings = true;
                        path_name_posting_begin = begin;
                        path_name_posting_end = end;
                    }
                }
            }
        } else if (name_only && term.value.size() == 2) {
            use_bigram_signature = true;
            add_required_gram(required_bigram_signature,
                              name_bigram_key(term.value[0], term.value[1]));
        } else if (name_only && term.value.size() >= 3) {
            for (std::size_t offset = 0; offset + 2 < term.value.size();
                 ++offset) {
                if (name_trigram_postings_.offsets.size() !=
                    name_trigram_bucket_count + 1) {
                    continue;
                }
                const auto bucket = name_trigram_key(
                    term.value[offset], term.value[offset + 1],
                    term.value[offset + 2]) & name_trigram_bucket_mask;
                const auto begin = name_trigram_postings_.offsets[bucket];
                const auto end = name_trigram_postings_.offsets[bucket + 1];
                if (!use_name_trigram_postings ||
                    end - begin < name_posting_end - name_posting_begin) {
                    use_name_trigram_postings = true;
                    name_posting_begin = begin;
                    name_posting_end = end;
                }
            }
        }
    }

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
    const auto accepted = [&](bool directory, std::wstring_view name,
                              std::wstring_view path, std::uint64_t size,
                              std::int64_t modified,
                              std::uint32_t attributes) {
        if (query.directories_only.has_value() &&
            directory != *query.directories_only) {
            return false;
        }
        return evaluate(query, terms, name, path, size, modified, attributes,
                        options.match_path, sensitive, whole_word);
    };
    const auto signature_contains = [](const NameGramSignature& candidate,
                                       const NameGramSignature& required) {
        for (std::size_t word = 0; word < candidate.words.size(); ++word) {
            if ((candidate.words[word] & required.words[word]) !=
                required.words[word]) {
                return false;
            }
        }
        return true;
    };

    // The GUI defaults to case-insensitive natural name order. Walking a
    // pre-sorted base order and merging the small delta overlay means we only
    // inspect records until the requested page is complete; the old path
    // materialized and sorted every match, which made one-character queries
    // take seconds on multi-million-file catalogs.
    if (options.sort == SortField::name && !sensitive &&
        query.duplicate_mode == DuplicateMode::none &&
        natural_name_order_.size() == records_.size()) {
        std::vector<Candidate> overlay_matches;
        overlay_matches.reserve(std::min(overlay_.size(), options.limit));
        for (const auto& [id, record] : overlay_) {
            if (accepted(record.directory, record.name, record.path,
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
            ? static_cast<std::size_t>(name_posting_end - name_posting_begin)
            : natural_name_order_.size();
        std::size_t order_position = options.descending ? base_order_size : 0;
        const auto next_base_match = [&]() -> std::optional<Candidate> {
            while ((!options.descending && order_position < base_order_size) ||
                   (options.descending && order_position != 0)) {
                const auto position = options.descending
                    ? --order_position : order_position++;
                const auto index = use_name_trigram_postings
                    ? name_trigram_postings_.record_indices[
                          name_posting_begin + position]
                    : natural_name_order_[position];
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
                if (use_path_trigram_signature &&
                    !signature_contains(path_trigram_signatures_[index],
                                        required_path_trigram_signature)) {
                    continue;
                }
                const auto name = name_view(record);
                const auto path = path_view(record);
                if (!accepted(record.directory, name, path, record.size,
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
    bool prefix_result_complete = false;
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
                const auto path = path_view(record);
                if (!accepted(record.directory, name, path, record.size,
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
                !accepted(record.directory, record.name, record.path,
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
            ? static_cast<std::size_t>(path_name_posting_end -
                                       path_name_posting_begin)
            : records_.size();
        for (std::size_t position = 0; position < candidate_count; ++position) {
            const auto index = use_path_name_trigram_postings
                ? name_trigram_postings_.record_indices[
                      path_name_posting_begin + position]
                : position;
            const auto& record = records_[index];
            if (std::binary_search(suppressed_base_ids_.begin(),
                                   suppressed_base_ids_.end(), record.id)) {
                continue;
            }
            const auto name = name_view(record);
            if (!text_match(path_term, name, sensitive, whole_word)) continue;
            const auto path = path_view(record);
            if (!accepted(record.directory, name, path, record.size,
                          record.last_write_time, record.attributes)) {
                continue;
            }
            consider({rank_record(terms, name, path, options.match_path,
                                  sensitive),
                      record.id, index, false});
        }
        for (const auto& [id, record] : overlay_) {
            if (!text_match(path_term, record.name, sensitive, whole_word) ||
                !accepted(record.directory, record.name, record.path,
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
                if (accepted(record.directory, record.name, record.path,
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
            const auto path = path_view(record);
            if (accepted(record.directory, name, path, record.size,
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
            for (std::uint32_t position = name_posting_begin;
                 position < name_posting_end; ++position) {
                const auto index =
                    name_trigram_postings_.record_indices[position];
                if (use_bigram_signature &&
                    !signature_contains(name_bigram_signatures_[index],
                                        required_bigram_signature)) {
                    continue;
                }
                if (use_path_trigram_signature &&
                    !signature_contains(path_trigram_signatures_[index],
                                        required_path_trigram_signature)) {
                    continue;
                }
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
                if (use_path_trigram_signature &&
                    !signature_contains(path_trigram_signatures_[index],
                                        required_path_trigram_signature)) {
                    continue;
                }
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
                if (accepted(record.directory, record.name, record.path,
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

#include "esm/index.hpp"
#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <mutex>
#include <queue>
#include <regex>
#include <optional>
#include <stdexcept>
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
std::uint32_t name_prefix_key(std::wstring_view value) noexcept {
    if (value.empty()) return 0;
    const auto first = static_cast<std::uint16_t>(normalize_char(value[0], false));
    const auto second = value.size() > 1
        ? static_cast<std::uint16_t>(normalize_char(value[1], false))
        : std::uint16_t{};
    return (static_cast<std::uint32_t>(first) << 16U) | second;
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
bool text_match(const CompiledTerm& term,std::wstring_view value,bool sensitive,bool whole_word){if(!term.match_diacritics){const auto normalized=normalize_match_text(value,sensitive,false);if(term.regex)return term.regex_pattern&&std::regex_search(normalized.begin(),normalized.end(),*term.regex_pattern);return term.wildcard?wildcard_text(term,normalized,true):contains_text(normalized,term,true,whole_word);}if(term.regex)return term.regex_pattern&&std::regex_search(value.begin(),value.end(),*term.regex_pattern);return term.wildcard?wildcard_text(term,value,sensitive):contains_text(value,term,sensitive,whole_word);}
bool exact_text_match(const CompiledTerm& term,std::wstring_view value,bool sensitive){if(!term.match_diacritics){const auto normalized=normalize_match_text(value,sensitive,false);return equal_text(normalized,term,true);}return equal_text(value,term,sensitive);}
bool prefix_text_match(const CompiledTerm& term,std::wstring_view value,bool sensitive){if(!term.match_diacritics){const auto normalized=normalize_match_text(value,sensitive,false);return starts_text(normalized,term,true);}return starts_text(value,term,sensitive);}
bool term_matches(const CompiledTerm& term,std::wstring_view name,std::wstring_view path,std::uint64_t size,std::int64_t modified,std::uint32_t attributes,bool match_path,bool sensitive,bool whole_word){switch(term.target){case MatchTarget::name:return text_match(term,name,sensitive,whole_word);case MatchTarget::path:return text_match(term,path,sensitive,whole_word);case MatchTarget::extension:{auto ext=extension_of(name);return term.regex||term.wildcard?text_match(term,ext,sensitive,whole_word):exact_text_match(term,ext,sensitive);}case MatchTarget::size:return compare_number(size,term);case MatchTarget::last_write_time:return compare_number(modified<0?0:(std::uint64_t)modified,term);case MatchTarget::attributes:{bool present=(attributes&term.attribute_mask)==term.attribute_mask;return term.attribute_absent?!present:present;}case MatchTarget::any:return text_match(term,name,sensitive,whole_word)||(match_path&&text_match(term,path,sensitive,whole_word));}return false;}
bool evaluate(const ParsedQuery& query,const std::vector<CompiledTerm>& terms,std::wstring_view name,std::wstring_view path,std::uint64_t size,std::int64_t modified,std::uint32_t attributes,bool match_path,bool sensitive,bool whole_word){if(query.program.empty())return true;std::vector<bool> stack;stack.reserve(query.terms.size());for(auto& instruction:query.program){if(instruction.opcode==QueryOpcode::term){if(instruction.term_index>=terms.size())return false;stack.push_back(term_matches(terms[instruction.term_index],name,path,size,modified,attributes,match_path,sensitive,whole_word));}else if(instruction.opcode==QueryOpcode::logical_not){if(stack.empty())return false;stack.back()=!stack.back();}else{if(stack.size()<2)return false;bool right=stack.back();stack.pop_back();bool left=stack.back();stack.back()=instruction.opcode==QueryOpcode::logical_and?(left&&right):(left||right);}}return stack.size()==1&&stack.back();}
int rank_record(const std::vector<CompiledTerm>& terms,std::wstring_view name,std::wstring_view path,bool match_path,bool sensitive){int score=0;for(auto& term:terms){if(term.excluded||term.target==MatchTarget::size||term.target==MatchTarget::last_write_time||term.target==MatchTarget::attributes)continue;if(term.regex||term.wildcard){score+=20;continue;}if(exact_text_match(term,name,sensitive))score+=120;else if(prefix_text_match(term,name,sensitive))score+=80;else if(text_match(term,name,sensitive,false))score+=50;else if(match_path&&text_match(term,path,sensitive,false))score+=20;}return score;}
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
    accelerators.trigram_signatures.resize(records.size());
    std::vector<std::uint64_t> prefix_entries;
    prefix_entries.reserve(records.size());

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
            for (std::size_t offset = 0; offset + 2 < value.size(); ++offset) {
                add_gram(accelerators.trigram_signatures[record_index],
                         name_trigram_key(value[offset], value[offset + 1],
                                          value[offset + 2]));
            }
        };
        add_name_grams(name);

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
            if (differs) add_name_grams(folded);
        }
    }

    std::sort(prefix_entries.begin(), prefix_entries.end());
    accelerators.prefix_order.reserve(prefix_entries.size());
    for (const auto entry : prefix_entries) {
        accelerators.prefix_order.push_back(
            static_cast<std::uint32_t>(entry));
    }

    std::size_t begin = 0;
    while (begin < prefix_entries.size()) {
        const auto key = static_cast<std::uint32_t>(prefix_entries[begin] >> 32U);
        std::size_t end = begin + 1;
        while (end < prefix_entries.size() &&
               static_cast<std::uint32_t>(prefix_entries[end] >> 32U) == key) {
            ++end;
        }
        accelerators.prefix_ranges.push_back({
            key, static_cast<std::uint32_t>(begin),
            static_cast<std::uint32_t>(end)});
        begin = end;
    }

    begin = 0;
    while (begin < prefix_entries.size()) {
        const auto key = static_cast<std::uint16_t>(prefix_entries[begin] >> 48U);
        std::size_t end = begin + 1;
        while (end < prefix_entries.size() &&
               static_cast<std::uint16_t>(prefix_entries[end] >> 48U) == key) {
            ++end;
        }
        accelerators.first_character_ranges.push_back({
            key, static_cast<std::uint32_t>(begin),
            static_cast<std::uint32_t>(end)});
        begin = end;
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
        name_trigram_signatures_.swap(name_accelerators.trigram_signatures);
        name_prefix_order_.swap(name_accelerators.prefix_order);
        name_prefix_ranges_.swap(name_accelerators.prefix_ranges);
        name_first_character_ranges_.swap(
            name_accelerators.first_character_ranges);
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
    name_trigram_signatures_ = std::move(name_accelerators.trigram_signatures);
    name_prefix_order_ = std::move(name_accelerators.prefix_order);
    name_prefix_ranges_ = std::move(name_accelerators.prefix_ranges);
    name_first_character_ranges_ =
        std::move(name_accelerators.first_character_ranges);
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
    std::priority_queue<Candidate, std::vector<Candidate>, MinScoreFirst> best;
    NameGramSignature required_bigram_signature{};
    NameGramSignature required_trigram_signature{};
    bool use_bigram_signature = false;
    bool use_trigram_signature = false;

    const auto add_required_gram = [](NameGramSignature& signature,
                                      std::uint32_t hash) {
        for (unsigned shift : {0U, 8U, 16U, 24U}) {
            const auto bit = (hash >> shift) & 0xffU;
            signature.words[bit >> 6U] |=
                std::uint64_t{1} << (bit & 63U);
        }
    };
    for (const auto term_index : mandatory_term_indices(query)) {
        if (term_index >= terms.size()) continue;
        const auto& term = terms[term_index];
        const bool name_only = term.target == MatchTarget::name ||
            (term.target == MatchTarget::any && !options.match_path);
        if (!name_only || term.regex || term.wildcard) continue;
        if (term.value.size() == 2) {
            use_bigram_signature = true;
            add_required_gram(required_bigram_signature,
                              name_bigram_key(term.value[0], term.value[1]));
        } else if (term.value.size() >= 3) {
            use_trigram_signature = true;
            for (std::size_t offset = 0; offset + 2 < term.value.size();
                 ++offset) {
                add_required_gram(
                    required_trigram_signature,
                    name_trigram_key(term.value[offset], term.value[offset + 1],
                                     term.value[offset + 2]));
            }
        }
    }

    const bool bounded = options.sort == SortField::relevance &&
        !options.descending && query.duplicate_mode == DuplicateMode::none;
    const auto consider = [&](Candidate candidate) {
        if (!bounded) {
            candidates.push_back(candidate);
            return;
        }
        if (best.size() < options.limit) {
            best.push(candidate);
        } else if (candidate.score > best.top().score ||
                   (candidate.score == best.top().score &&
                    candidate.id < best.top().id)) {
            best.pop();
            best.push(candidate);
        }
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

    // A plain one-term relevance query is the GUI's hot path. Exact and
    // prefix matches always outrank substring-only matches, so if the prefix
    // range already fills the requested page, the rest of the catalog cannot
    // affect the result set.
    bool prefix_result_complete = false;
    const bool simple_prefix_query = options.match_diacritics && bounded &&
        query.terms.size() == 1 &&
        query.program.size() == 1 &&
        query.program.front().opcode == QueryOpcode::term &&
        query.program.front().term_index == 0 && !terms[0].excluded &&
        !terms[0].regex && !terms[0].wildcard && !terms[0].value.empty() &&
        (terms[0].target == MatchTarget::name ||
         (terms[0].target == MatchTarget::any && !options.match_path));
    if (simple_prefix_query) {
        const auto& prefix_term = terms[0];
        std::uint32_t prefix_begin = 0;
        std::uint32_t prefix_end = 0;
        if (prefix_term.value.size() == 1) {
            const auto key = static_cast<std::uint16_t>(
                normalize_char(prefix_term.value[0], false));
            const auto found = std::lower_bound(
                name_first_character_ranges_.begin(),
                name_first_character_ranges_.end(), key,
                [](const NameFirstCharacterRange& range, std::uint16_t value) {
                    return range.key < value;
                });
            if (found != name_first_character_ranges_.end() &&
                found->key == key) {
                prefix_begin = found->begin;
                prefix_end = found->end;
            }
        } else {
            const auto key = name_prefix_key(prefix_term.value);
            const auto found = std::lower_bound(
                name_prefix_ranges_.begin(), name_prefix_ranges_.end(), key,
                [](const NamePrefixRange& range, std::uint32_t value) {
                    return range.key < value;
                });
            if (found != name_prefix_ranges_.end() && found->key == key) {
                prefix_begin = found->begin;
                prefix_end = found->end;
            }
        }

        std::size_t accepted_prefixes = 0;
        for (std::uint32_t position = prefix_begin; position < prefix_end;
             ++position) {
            const auto index = name_prefix_order_[position];
            const auto& record = records_[index];
            if (std::binary_search(suppressed_base_ids_.begin(),
                                   suppressed_base_ids_.end(), record.id)) {
                continue;
            }
            const auto name = name_view(record);
            if (!starts_text(name, prefix_term, sensitive)) continue;
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
        for (const auto& [id, record] : overlay_) {
            if (!starts_text(record.name, prefix_term, sensitive) ||
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

    if (!prefix_result_complete) {
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

        // Overlay IDs are not ordered, so account for all of them before the
        // ordered base scan. Prefix overlay records were already considered.
        if (simple_prefix_query) {
            for (const auto& [id, record] : overlay_) {
                if (starts_text(record.name, terms[0], sensitive)) continue;
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
            while (suppressed < suppressed_base_ids_.size() &&
                   suppressed_base_ids_[suppressed] < record.id) {
                ++suppressed;
            }
            if (suppressed < suppressed_base_ids_.size() &&
                suppressed_base_ids_[suppressed] == record.id) {
                return;
            }
            const auto name = name_view(record);
            if (simple_prefix_query && starts_text(name, terms[0], sensitive)) {
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
            return simple_prefix_query && best.size() >= options.limit &&
                best.top().score >= 50 && records_[index].id > best.top().id;
        };

        if (use_bigram_signature || use_trigram_signature) {
            for (std::size_t index = 0; index < records_.size(); ++index) {
                if (remaining_base_cannot_improve(index)) break;
                if (use_bigram_signature &&
                    !signature_contains(name_bigram_signatures_[index],
                                        required_bigram_signature)) {
                    continue;
                }
                if (use_trigram_signature &&
                    !signature_contains(name_trigram_signatures_[index],
                                        required_trigram_signature)) {
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
        if (!simple_prefix_query) {
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

    if (bounded) {
        candidates.reserve(best.size());
        while (!best.empty()) {
            candidates.push_back(best.top());
            best.pop();
        }
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

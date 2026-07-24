#pragma once
#include "esm/file_record.hpp"
#include "esm/query.hpp"
#include <cstddef>
#include <array>
#include <cstdint>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace esm {
enum class SortField {
    relevance,
    name,
    path,
    size,
    last_write_time,
    attributes,
    extension,
    type,
    creation_time,
    last_access_time,
    change_time,
    run_count,
    last_open_time,
    file_list_name
};
struct SearchOptions {
    std::size_t limit{100};
    bool case_sensitive{};
    bool match_path{};
    bool whole_word{};
    bool match_diacritics{true};
    SortField sort{SortField::relevance};
    bool descending{};
};
struct SearchResult { FileRecord record; int score{}; };
class MetadataIndex {
public:
    static constexpr std::size_t default_auto_compaction_threshold = 100'000;

    explicit MetadataIndex(
        std::size_t auto_compaction_threshold =
            default_auto_compaction_threshold)
        : auto_compaction_threshold_(auto_compaction_threshold) {}

    void replace(const std::vector<FileRecord>& records);
    void apply_delta(std::vector<FileRecord> upserts,
                     const std::vector<std::uint64_t>& removed_ids);
    [[nodiscard]] std::vector<SearchResult> search(std::wstring_view query, const SearchOptions& options = {}) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t pending_delta_size() const;
    [[nodiscard]] bool compact();
    void set_auto_compaction_threshold(std::size_t threshold);
    [[nodiscard]] std::size_t compaction_count() const;
private:
    struct CompactRecord {
        std::uint64_t id{};
        std::uint64_t parent_id{};
        std::uint64_t size{};
        std::int64_t last_write_time{};
        std::uint32_t attributes{};
        std::uint32_t path_offset{};
        std::uint32_t path_length{};
        std::uint32_t name_offset{};
        std::uint32_t name_length{};
        bool directory{};
    };
    [[nodiscard]] std::wstring_view path_view(const CompactRecord& record) const;
    [[nodiscard]] std::wstring_view name_view(const CompactRecord& record) const;
    [[nodiscard]] FileRecord materialize(const CompactRecord& record) const;
    struct NameGramSignature {
        std::array<std::uint64_t, 4> words{};
    };
    struct NamePrefixRange {
        std::uint32_t key{};
        std::uint32_t begin{};
        std::uint32_t end{};
    };
    struct NameFirstCharacterRange {
        std::uint16_t key{};
        std::uint32_t begin{};
        std::uint32_t end{};
    };
    struct NameTrigramPostingIndex {
        std::vector<std::uint32_t> offsets;
        std::vector<std::uint32_t> record_indices;
    };
    struct NameSearchAccelerators {
        std::vector<NameGramSignature> bigram_signatures;
        NameTrigramPostingIndex trigram_postings;
        // Explicit path: queries are otherwise forced to touch every full
        // path string. A single trigram Bloom signature per record keeps the
        // common path-substring case on a contiguous, metadata-only scan.
        std::vector<NameGramSignature> path_trigram_signatures;
        // Base records in the exact case-insensitive natural name/path/id
        // order used by the default GUI sort. Queries can walk this order and
        // stop after one page instead of sorting every match.
        std::vector<std::uint32_t> natural_name_order;
        std::vector<std::uint32_t> prefix_order;
        std::vector<NamePrefixRange> prefix_ranges;
        std::vector<NameFirstCharacterRange> first_character_ranges;
        // Only names whose accent-folded form differs are included here.
        // This preserves ignore-diacritics prefix semantics without forcing
        // the common ASCII query path to normalize every catalog entry.
        std::vector<std::uint32_t> folded_prefix_order;
        std::vector<NamePrefixRange> folded_prefix_ranges;
        std::vector<NameFirstCharacterRange> folded_first_character_ranges;
    };

    [[nodiscard]] static NameSearchAccelerators
    build_name_search_accelerators(
        const std::vector<CompactRecord>& records,
        const std::vector<wchar_t>& strings);
    [[nodiscard]] bool base_contains(std::uint64_t id) const;
    void compact_locked();
    void rebuild_suppressed_base_ids_locked();
    mutable std::shared_mutex mutex_;
    std::vector<CompactRecord> records_;
    std::vector<wchar_t> strings_;
    std::vector<NameGramSignature> name_bigram_signatures_;
    NameTrigramPostingIndex name_trigram_postings_;
    std::vector<NameGramSignature> path_trigram_signatures_;
    std::vector<std::uint32_t> natural_name_order_;
    std::vector<std::uint32_t> name_prefix_order_;
    std::vector<NamePrefixRange> name_prefix_ranges_;
    std::vector<NameFirstCharacterRange> name_first_character_ranges_;
    std::vector<std::uint32_t> folded_name_prefix_order_;
    std::vector<NamePrefixRange> folded_name_prefix_ranges_;
    std::vector<NameFirstCharacterRange> folded_name_first_character_ranges_;
    std::unordered_map<std::uint64_t, FileRecord> overlay_;
    std::unordered_set<std::uint64_t> removed_;
    std::vector<std::uint64_t> suppressed_base_ids_;
    std::size_t live_size_{};
    std::size_t auto_compaction_threshold_{};
    std::size_t compaction_count_{};
};
}

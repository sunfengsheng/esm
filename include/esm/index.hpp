#pragma once
#include "esm/file_record.hpp"
#include "esm/ntfs_catalog.hpp"
#include "esm/query.hpp"
#include "esm/usn_journal.hpp"
#include <cstddef>
#include <array>
#include <cstdint>
#include <shared_mutex>
#include <span>
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
struct MetadataIndexStorageStats {
    std::size_t base_records{};
    std::size_t name_only_paths{};
    std::size_t string_characters{};
    std::size_t record_bytes{};
    std::size_t string_bytes{};
    std::size_t signature_bytes{};
    std::size_t path_signature_count{};
    std::size_t path_signature_owner_bytes{};
    std::size_t posting_entries{};
    std::size_t posting_bytes{};
    std::size_t ordering_bytes{};
    std::size_t total_base_capacity_bytes{};
};
struct MetadataHydrationBatch {
    std::vector<FileRecord> records;
    std::size_t examined{};
    std::uint64_t next_id{};
    bool complete{};
};
struct MetadataApplyStats {
    std::size_t attempted{};
    std::size_t applied{};
    std::size_t stale{};
};
struct MetadataReuseStats {
    std::size_t examined{};
    std::size_t reused{};
    std::size_t unknown{};
    std::size_t stale{};
};
struct MetadataCatalogSnapshot {
    std::vector<CatalogBaseNode> nodes;
    std::vector<wchar_t> names;
};
class MetadataIndex {
public:
    static constexpr std::size_t default_auto_compaction_threshold = 100'000;

    explicit MetadataIndex(
        std::size_t auto_compaction_threshold =
            default_auto_compaction_threshold)
        : auto_compaction_threshold_(auto_compaction_threshold) {}

    void replace(const std::vector<FileRecord>& records);
    // Consumes and releases the source record vector after compact path data
    // has been copied, before the expensive search accelerators are built.
    void replace(std::vector<FileRecord>&& records);
    void apply_delta(std::vector<FileRecord> upserts,
                     const std::vector<std::uint64_t>& removed_ids);
    // Applies raw per-volume USN changes directly to the searchable base.
    // IDs are namespaced internally so the multi-volume service does not need
    // to retain a duplicate NtfsCatalog for every indexed volume.
    void apply_ntfs_changes(std::wstring_view volume_identity,
                            std::wstring_view volume_root,
                            std::uint64_t root_id,
                            const UsnChangeBatch& batch,
                            bool hydrate_search_metadata = true);
    [[nodiscard]] std::vector<SearchResult> search(std::wstring_view query, const SearchOptions& options = {}) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t pending_delta_size() const;
    // Materializes a bounded, ID-ordered base batch for filesystem metadata
    // I/O outside the index lock. Overlay and already-hydrated records are
    // skipped; sparse scans may examine beyond limit while returning at most
    // limit unresolved records, reducing empty durable WAL transactions.
    [[nodiscard]] MetadataHydrationBatch metadata_hydration_batch(
        std::uint64_t after_id, std::size_t limit) const;
    // Merges size/write-time/attribute updates only when the indexed path still
    // matches the path that was hydrated, preventing rename races.
    [[nodiscard]] MetadataApplyStats apply_search_metadata(
        std::span<const FileRecord> updates);
    // Sorts a new reconciliation baseline by ID and copies already-hydrated
    // size/write-time/attributes from the current live view only when file ID
    // and complete path still match. Unknown or stale records remain untouched.
    [[nodiscard]] MetadataReuseStats reuse_search_metadata(
        std::span<FileRecord> records) const;
    // Materializes the current live base + overlay view, excluding removals,
    // in stable ID order for a durable checkpoint consolidation.
    [[nodiscard]] std::vector<FileRecord> snapshot_records() const;
    // Exports the current live view in the compact version-3 component/anchor persistence
    // layout. Only fixed-size node metadata, component names, and the small
    // set of required full-path anchors are copied; search accelerators and
    // complete paths for ordinary descendants are not duplicated.
    [[nodiscard]] MetadataCatalogSnapshot catalog_snapshot() const;
    [[nodiscard]] bool compact();
    void set_auto_compaction_threshold(std::size_t threshold);
    [[nodiscard]] std::size_t compaction_count() const;
    [[nodiscard]] MetadataIndexStorageStats storage_stats() const;
private:
    static constexpr std::uint32_t missing_parent_index = 0x7fffffffU;
    struct CompactRecord {
        std::uint64_t id{};
        std::uint64_t size{};
        std::int64_t last_write_time{};
        std::uint32_t attributes{};
        std::uint32_t path_offset{};
        std::uint32_t path_length : 16 {};
        std::uint32_t name_length_value : 16 {};
        std::uint32_t parent_index : 31 {missing_parent_index};
        std::uint32_t name_only_path : 1 {};

        [[nodiscard]] std::uint32_t name_length() const noexcept {
            return name_length_value;
        }
        [[nodiscard]] bool directory() const noexcept {
            return (attributes & 0x10U) != 0;
        }
        [[nodiscard]] std::uint32_t name_offset() const noexcept {
            return path_offset + path_length - name_length_value;
        }
    };
    static_assert(sizeof(CompactRecord) == 40);
    struct ParentIdAnchor {
        std::uint64_t id{};
        std::uint64_t parent_id{};
    };
    struct PendingCompactRecord {
        CompactRecord record;
        std::uint64_t parent_id{};
    };
    [[nodiscard]] std::wstring_view path_view(
        const CompactRecord& record, std::wstring& scratch) const;
    [[nodiscard]] std::wstring_view name_view(const CompactRecord& record) const;
    [[nodiscard]] FileRecord materialize(const CompactRecord& record) const;
    [[nodiscard]] std::uint64_t parent_id(const CompactRecord& record) const;
    struct NameBigramSignature {
        std::array<std::uint64_t, 2> words{};
    };
    struct PathTrigramSignature {
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
        // Each bucket stores monotonically increasing positions in
        // natural_name_order, delta encoded as unsigned varints.
        std::vector<std::uint32_t> byte_offsets;
        std::vector<std::uint32_t> counts;
        std::vector<std::uint8_t> encoded_positions;
    };
    struct PathSignatureFallback {
        std::uint32_t record_index{};
        std::uint32_t signature_index{};
    };
    struct NameSearchAccelerators {
        std::vector<NameBigramSignature> bigram_signatures;
        NameTrigramPostingIndex trigram_postings;
        // Explicit path: queries are otherwise forced to touch every full
        // path string. A single trigram Bloom signature per record keeps the
        // common path-substring case on a contiguous, metadata-only scan.
        // Full directory path signatures are shared by all direct children.
        // Directory membership plus a prefix-rank table derives the common
        // owner without storing one uint32 per record. Only unusual full-path
        // file anchors need a sparse explicit mapping.
        std::vector<PathTrigramSignature> path_trigram_signatures;
        std::vector<std::uint64_t> directory_signature_bits;
        std::vector<std::uint32_t> directory_signature_rank_prefix;
        std::vector<PathSignatureFallback> path_signature_fallbacks;
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

    void replace_impl(const std::vector<FileRecord>& records,
                      std::vector<FileRecord>* consumable_records);
    static void compact_base_paths(std::vector<CompactRecord>& records,
                                   std::vector<wchar_t>& strings);
    static void finalize_pending_records(
        std::vector<PendingCompactRecord>& pending,
        std::vector<CompactRecord>& records,
        std::vector<ParentIdAnchor>& parent_id_anchors);
    [[nodiscard]] static NameSearchAccelerators
    build_name_search_accelerators(
        const std::vector<CompactRecord>& records,
        const std::vector<wchar_t>& strings);
    [[nodiscard]] bool base_contains(std::uint64_t id) const;
    [[nodiscard]] std::uint32_t directory_signature_index(
        std::size_t record_index) const noexcept;
    [[nodiscard]] std::uint32_t path_signature_owner(
        std::size_t record_index) const noexcept;
    void compact_locked();
    void rebuild_suppressed_base_ids_locked();
    mutable std::shared_mutex mutex_;
    std::vector<CompactRecord> records_;
    std::vector<ParentIdAnchor> parent_id_anchors_;
    std::vector<wchar_t> strings_;
    std::vector<NameBigramSignature> name_bigram_signatures_;
    NameTrigramPostingIndex name_trigram_postings_;
    std::vector<PathTrigramSignature> path_trigram_signatures_;
    std::vector<std::uint64_t> directory_signature_bits_;
    std::vector<std::uint32_t> directory_signature_rank_prefix_;
    std::vector<PathSignatureFallback> path_signature_fallbacks_;
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

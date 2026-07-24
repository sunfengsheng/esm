#pragma once

#include "esm/file_record.hpp"
#include "esm/usn_journal.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace esm {
struct CatalogBaseNode {
    std::uint64_t id{};
    std::uint64_t parent_id{};
    std::uint64_t size{};
    std::int64_t last_write_time{};
    std::uint32_t attributes{};
    std::uint32_t name_offset{};
    std::uint32_t name_length{};
    std::uint8_t directory{};
    std::uint8_t reserved[3]{};
};

struct CatalogMappedBase {
    std::shared_ptr<const void> owner;
    const CatalogBaseNode* nodes{};
    std::size_t node_count{};
    const wchar_t* names{};
    std::size_t name_count{};
    std::size_t mapped_bytes{};
};

struct CatalogApplyResult {
    std::size_t created{};
    std::size_t updated{};
    std::size_t renamed{};
    std::size_t deleted{};
    std::size_t ignored{};
    std::vector<FileRecord> upserts;
    std::vector<std::uint64_t> removed_ids;
};

struct CatalogStorageStats {
    std::size_t live_nodes{};
    std::size_t base_nodes{};
    std::size_t base_name_chars{};
    std::size_t overlay_nodes{};
    std::size_t tombstones{};
    std::size_t compact_storage_bytes{};
    std::size_t compaction_count{};
    bool mapped_base{};
    std::size_t mapped_file_bytes{};
};

class CatalogStorageReadView {
public:
    CatalogStorageReadView() = default;
    CatalogStorageReadView(const CatalogStorageReadView&) = delete;
    CatalogStorageReadView& operator=(const CatalogStorageReadView&) = delete;
    CatalogStorageReadView(CatalogStorageReadView&&) noexcept = default;
    CatalogStorageReadView& operator=(CatalogStorageReadView&&) noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept { return complete_; }
    [[nodiscard]] std::span<const CatalogBaseNode> nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::span<const wchar_t> names() const noexcept {
        return names_;
    }

private:
    friend class NtfsCatalog;
    std::shared_lock<std::shared_mutex> lock_;
    std::span<const CatalogBaseNode> nodes_;
    std::span<const wchar_t> names_;
    bool complete_{};
};

class NtfsCatalog {
public:
    static constexpr std::size_t default_auto_compaction_threshold = 100'000;

    NtfsCatalog(
        std::wstring volume_root,
        std::uint64_t root_id,
        std::size_t auto_compaction_threshold =
            default_auto_compaction_threshold);

    void replace(const std::vector<FileRecord>& records);
    [[nodiscard]] bool replace_mapped(CatalogMappedBase storage);
    // Copies a mapped immutable base into owned vectors and releases the file
    // mapping. This is required before atomically replacing the mapped file.
    [[nodiscard]] bool materialize_mapped_base();
    [[nodiscard]] CatalogApplyResult apply(const UsnChangeBatch& batch);
    [[nodiscard]] std::vector<FileRecord> snapshot() const;
    // Returns the compact persistence form: node metadata and names only.
    // Paths are reconstructed from parent IDs when a snapshot is loaded.
    [[nodiscard]] std::vector<FileRecord> storage_snapshot() const;
    // Pins the compact base storage without copying it. The view is complete
    // only when there is no pending overlay or tombstone; callers that need a
    // persistence image should compact first and keep the view alive while
    // reading its spans.
    [[nodiscard]] CatalogStorageReadView storage_view() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t pending_delta_size() const;
    [[nodiscard]] bool compact();
    void set_auto_compaction_threshold(std::size_t threshold);
    [[nodiscard]] CatalogStorageStats storage_stats() const;

private:
    using BaseNode = CatalogBaseNode;

    struct Node {
        std::uint64_t id{};
        std::uint64_t parent_id{};
        std::uint64_t size{};
        std::int64_t last_write_time{};
        std::uint32_t attributes{};
        bool directory{};
        std::wstring name;
    };

    struct NodeView {
        std::uint64_t id{};
        std::uint64_t parent_id{};
        std::uint64_t size{};
        std::int64_t last_write_time{};
        std::uint32_t attributes{};
        bool directory{};
        std::wstring_view name;
    };

    [[nodiscard]] const BaseNode* find_base_unlocked(
        std::uint64_t id) const;
    [[nodiscard]] std::wstring_view base_name_unlocked(
        const BaseNode& node) const;
    [[nodiscard]] NodeView view_unlocked(const BaseNode& node) const;
    [[nodiscard]] static NodeView view_unlocked(const Node& node);
    [[nodiscard]] bool try_get_unlocked(
        std::uint64_t id, NodeView& result) const;
    [[nodiscard]] Node materialize_unlocked(const BaseNode& node) const;
    [[nodiscard]] Node& promote_unlocked(std::uint64_t id);
    [[nodiscard]] std::span<const BaseNode> base_nodes_unlocked() const;
    [[nodiscard]] std::span<const wchar_t> base_names_unlocked() const;
    [[nodiscard]] bool should_compact_unlocked() const;
    void materialize_mapped_base_unlocked();
    void compact_unlocked();

    std::wstring volume_root_;
    std::uint64_t root_id_{};
    mutable std::shared_mutex mutex_;
    std::vector<BaseNode> base_nodes_;
    std::vector<wchar_t> base_names_;
    CatalogMappedBase mapped_base_;
    std::unordered_map<std::uint64_t, Node> overlay_;
    std::unordered_set<std::uint64_t> tombstones_;
    std::size_t live_size_{};
    std::size_t auto_compaction_threshold_{};
    std::size_t compaction_count_{};
};
} // namespace esm

#pragma once

#include "esm/file_record.hpp"
#include "esm/journal_checkpoint.hpp"
#include "esm/ntfs_catalog.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace esm {
struct MetadataSnapshot {
    JournalCheckpoint checkpoint;
    std::uint64_t root_id{};
    std::wstring volume;
    std::vector<FileRecord> records;
};

struct MetadataSnapshotIoResult {
    bool ok{};
    std::uint32_t error{};
};

struct MetadataSnapshotLoadResult : MetadataSnapshotIoResult {
    MetadataSnapshot snapshot;
};

struct MappedMetadataSnapshot {
    JournalCheckpoint checkpoint;
    std::uint64_t root_id{};
    std::wstring volume;
    CatalogMappedBase catalog;
    std::uint32_t format_version{};
};

struct MappedMetadataSnapshotLoadResult : MetadataSnapshotIoResult {
    MappedMetadataSnapshot snapshot;
};

[[nodiscard]] std::filesystem::path metadata_snapshot_path(
    const std::filesystem::path& checkpoint_path);
[[nodiscard]] MetadataSnapshotIoResult save_metadata_snapshot_atomic(
    const std::filesystem::path& path,
    const MetadataSnapshot& snapshot);
// Writes the version-2 format directly from the catalog's compact node and
// name arenas, avoiding a temporary vector<FileRecord> and duplicate strings.
// The catalog must have no pending overlay/tombstones.
[[nodiscard]] MetadataSnapshotIoResult save_metadata_catalog_snapshot_atomic(
    const std::filesystem::path& path,
    JournalCheckpoint checkpoint,
    std::uint64_t root_id,
    std::wstring_view volume,
    const NtfsCatalog& catalog);
// Writes an already-exported compact catalog. The node IDs must be strictly
// increasing and every name range must refer to the supplied name arena.
[[nodiscard]] MetadataSnapshotIoResult save_metadata_catalog_snapshot_atomic(
    const std::filesystem::path& path,
    JournalCheckpoint checkpoint,
    std::uint64_t root_id,
    std::wstring_view volume,
    std::span<const CatalogBaseNode> nodes,
    std::span<const wchar_t> names);
// Writes the version-3 multi-root index format. reserved[0] marks entries
// whose string is a complete path anchor; other entries store only a name and
// reconstruct their path through parent_id.
[[nodiscard]] MetadataSnapshotIoResult save_metadata_index_snapshot_atomic(
    const std::filesystem::path& path,
    JournalCheckpoint checkpoint,
    std::uint64_t root_id,
    std::wstring_view volume,
    std::span<const CatalogBaseNode> nodes,
    std::span<const wchar_t> text);
[[nodiscard]] MetadataSnapshotLoadResult load_metadata_snapshot(
    const std::filesystem::path& path);
// Loads version-2 catalog and version-3 multi-root index snapshots as a
// read-only file mapping. Version-1 snapshots return ERROR_REVISION_MISMATCH
// so callers can use the legacy loader. Callers must inspect format_version
// before handing the mapped nodes to NtfsCatalog; version 3 contains anchors.
[[nodiscard]] MappedMetadataSnapshotLoadResult load_metadata_snapshot_mapped(
    const std::filesystem::path& path);
} // namespace esm

#pragma once
#include "esm/index.hpp"
#include "esm/ntfs_catalog.hpp"
#include "esm/usn_journal.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
namespace esm {
struct MetadataWalResult {
    bool ok{};
    bool torn_tail{};
    std::uint32_t error{};
    std::size_t transactions{};
    std::size_t changes{};
    std::uint64_t valid_bytes{};
    std::uint64_t discarded_tail_bytes{};
    std::int64_t next_usn{};
};
struct MetadataWalBinding {
    std::uint64_t generation{};
    std::wstring volume_identity;
    std::wstring volume_root;
    std::uint64_t root_id{};
};
[[nodiscard]] std::filesystem::path metadata_wal_path(
    const std::filesystem::path& checkpoint_path);
// Derives a generation- and volume-bound name/USN delta WAL path for the
// multi-volume MFT database. The file contents independently validate the same
// binding so a renamed or colliding path cannot be replayed into another base.
[[nodiscard]] std::filesystem::path metadata_wal_path(
    const std::filesystem::path& snapshot_path,
    std::uint64_t generation,
    std::wstring_view volume_identity);
[[nodiscard]] MetadataWalResult append_metadata_wal(
    const std::filesystem::path& path,
    std::uint64_t journal_id,
    std::int64_t start_usn,
    const UsnChangeBatch& batch);
[[nodiscard]] MetadataWalResult append_metadata_wal(
    const std::filesystem::path& path,
    const MetadataWalBinding& binding,
    std::uint64_t journal_id,
    std::int64_t start_usn,
    const UsnChangeBatch& batch);
[[nodiscard]] MetadataWalResult replay_metadata_wal(
    const std::filesystem::path& path,
    std::uint64_t journal_id,
    std::int64_t start_usn,
    NtfsCatalog& catalog,
    MetadataIndex& index);
[[nodiscard]] MetadataWalResult replay_metadata_wal(
    const std::filesystem::path& path,
    const MetadataWalBinding& binding,
    std::uint64_t journal_id,
    std::int64_t start_usn,
    MetadataIndex& index);
[[nodiscard]] MetadataWalResult reset_metadata_wal(
    const std::filesystem::path& path);
}

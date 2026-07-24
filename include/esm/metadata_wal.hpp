#pragma once
#include "esm/index.hpp"
#include "esm/ntfs_catalog.hpp"
#include "esm/usn_journal.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
[[nodiscard]] std::filesystem::path metadata_wal_path(
    const std::filesystem::path& checkpoint_path);
[[nodiscard]] MetadataWalResult append_metadata_wal(
    const std::filesystem::path& path,
    std::uint64_t journal_id,
    std::int64_t start_usn,
    const UsnChangeBatch& batch);
[[nodiscard]] MetadataWalResult replay_metadata_wal(
    const std::filesystem::path& path,
    std::uint64_t journal_id,
    std::int64_t start_usn,
    NtfsCatalog& catalog,
    MetadataIndex& index);
[[nodiscard]] MetadataWalResult reset_metadata_wal(
    const std::filesystem::path& path);
}

#pragma once

#include "esm/file_record.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace esm {
struct MetadataHydrationWalUpdate {
    std::uint64_t id{};
    std::uint64_t path_fingerprint{};
    std::uint64_t size{};
    std::int64_t last_write_time{};
    std::uint32_t attributes{};
};

struct MetadataHydrationWalResult {
    bool ok{};
    bool torn_tail{};
    bool generation_mismatch{};
    bool complete{};
    std::uint32_t error{};
    std::uint64_t generation{};
    std::uint64_t next_id{};
    std::size_t transactions{};
    std::size_t examined{};
    std::size_t updates{};
    std::size_t applied{};
    std::size_t stale{};
    std::uint64_t valid_bytes{};
    std::uint64_t discarded_tail_bytes{};
};

[[nodiscard]] std::uint64_t metadata_path_fingerprint(
    std::wstring_view path) noexcept;
[[nodiscard]] MetadataHydrationWalUpdate make_metadata_hydration_wal_update(
    const FileRecord& record) noexcept;
[[nodiscard]] std::filesystem::path metadata_hydration_wal_path(
    const std::filesystem::path& snapshot_path);
[[nodiscard]] MetadataHydrationWalResult initialize_metadata_hydration_wal(
    const std::filesystem::path& path, std::uint64_t generation);
[[nodiscard]] MetadataHydrationWalResult append_metadata_hydration_wal(
    const std::filesystem::path& path,
    std::uint64_t generation,
    std::uint64_t after_id,
    std::uint64_t next_id,
    std::size_t examined,
    std::span<const MetadataHydrationWalUpdate> updates,
    bool complete);
// Replays valid transactions into records. The vector is sorted by ID in
// place, which is also the order consumed by MetadataIndex::replace().
[[nodiscard]] MetadataHydrationWalResult replay_metadata_hydration_wal(
    const std::filesystem::path& path,
    std::uint64_t generation,
    std::vector<FileRecord>& records);
} // namespace esm
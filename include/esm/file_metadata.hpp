#pragma once

#include "esm/file_record.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace esm {
// Refreshes the compact-search metadata retained by the base index: size,
// last-write time, attributes, and directory state. Existing indexed values
// are left untouched when the path is no longer accessible.
[[nodiscard]] bool hydrate_file_search_metadata(FileRecord& record) noexcept;

struct MetadataHydrationStats {
    std::size_t attempted{};
    std::size_t hydrated{};
    std::size_t errors{};
};

// Hydrates records in place with a bounded worker pool and without allocating
// another record vector. A worker_count of zero selects a conservative default.
// When supplied, succeeded must match records.size() and receives one byte per
// successfully refreshed record.
[[nodiscard]] MetadataHydrationStats hydrate_file_search_metadata_records(
    std::span<FileRecord> records, std::size_t worker_count = 0,
    std::span<std::uint8_t> succeeded = {});

// Refreshes size, creation/access/write/change times, attributes, and
// directory state from the filesystem. Existing indexed values are left
// untouched when the path is no longer accessible.
[[nodiscard]] bool hydrate_file_metadata(FileRecord& record) noexcept;
} // namespace esm

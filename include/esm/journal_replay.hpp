#pragma once

#include "esm/index.hpp"
#include "esm/journal_checkpoint.hpp"
#include "esm/ntfs_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace esm {
struct JournalReplayResult {
    std::size_t created{};
    std::size_t updated{};
    std::size_t renamed{};
    std::size_t deleted{};
    std::size_t ignored{};
    std::size_t index_upserts{};
    std::size_t index_removals{};
    std::int64_t next_usn{};
    bool checkpoint_saved{};
    std::uint32_t error{};
};

// Applies one batch in at-least-once-safe order:
// catalog -> searchable overlay -> durable checkpoint.
[[nodiscard]] JournalReplayResult replay_journal_batch(
    NtfsCatalog& catalog,
    MetadataIndex& index,
    const UsnJournalState& journal,
    const UsnChangeBatch& batch,
    const std::filesystem::path& checkpoint_path,
    const std::filesystem::path& wal_path = {},
    std::int64_t wal_start_usn = 0);
} // namespace esm

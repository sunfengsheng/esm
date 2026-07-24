#pragma once

#include "esm/usn_journal.hpp"

#include <cstdint>
#include <filesystem>

namespace esm {
struct JournalCheckpoint {
    std::uint64_t journal_id{};
    std::int64_t next_usn{};
};

enum class CheckpointStatus {
    valid,
    journal_changed,
    expired,
    ahead_of_journal,
};

struct CheckpointIoResult {
    bool ok{};
    std::uint32_t error{};
};

struct CheckpointLoadResult : CheckpointIoResult {
    JournalCheckpoint checkpoint;
};

struct PathVolumeComparison {
    bool ok{};
    bool same_volume{};
    std::uint32_t error{};
};

[[nodiscard]] CheckpointStatus validate_checkpoint(
    const UsnJournalState& journal,
    const JournalCheckpoint& checkpoint) noexcept;
[[nodiscard]] PathVolumeComparison compare_path_volumes(
    const std::filesystem::path& left,
    const std::filesystem::path& right);
[[nodiscard]] CheckpointLoadResult load_checkpoint(
    const std::filesystem::path& path);
[[nodiscard]] CheckpointIoResult save_checkpoint_atomic(
    const std::filesystem::path& path,
    const JournalCheckpoint& checkpoint);
} // namespace esm

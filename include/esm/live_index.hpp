#pragma once

#include "esm/index.hpp"
#include "esm/journal_checkpoint.hpp"
#include "esm/metadata_snapshot.hpp"
#include "esm/metadata_wal.hpp"
#include "esm/ntfs_catalog.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

namespace esm {
inline constexpr std::uint64_t default_wal_checkpoint_bytes =
    64ULL * 1024ULL * 1024ULL;

enum class LiveStartStage {
    none,
    volume_check,
    journal_query,
    snapshot_load,
    mft_enumeration,
    index_build,
    checkpoint_save,
    catch_up,
    snapshot_save,
};

struct LivePollResult {
    bool ok{};
    bool rebuild_required{};
    CheckpointStatus checkpoint_status{CheckpointStatus::valid};
    std::uint32_t error{};
    std::int64_t next_usn{};
    std::size_t batches{};
    std::size_t changes{};
    std::size_t created{};
    std::size_t updated{};
    std::size_t renamed{};
    std::size_t deleted{};
    std::size_t ignored{};
    bool checkpoint_consolidated{};
    std::uint64_t wal_bytes{};
    std::chrono::milliseconds checkpoint_elapsed{};
};

struct LiveSnapshotSaveResult {
    bool ok{};
    std::uint32_t error{};
    std::size_t entries{};
    std::chrono::milliseconds elapsed{};
};

struct LiveStartResult {
    bool ok{};
    bool checkpoint_same_volume{};
    bool snapshot_loaded{};
    bool snapshot_rejected{};
    bool snapshot_saved{};
    LiveStartStage stage{LiveStartStage::none};
    CheckpointStatus snapshot_checkpoint_status{CheckpointStatus::valid};
    std::uint32_t error{};
    std::uint32_t snapshot_error{};
    std::uint64_t journal_id{};
    std::int64_t bootstrap_usn{};
    std::size_t entries{};
    std::size_t scan_errors{};
    std::size_t metadata_hydrated{};
    std::size_t metadata_errors{};
    std::chrono::milliseconds snapshot_load_elapsed{};
    std::chrono::milliseconds enumeration_elapsed{};
    std::chrono::milliseconds metadata_elapsed{};
    std::chrono::milliseconds index_elapsed{};
    std::chrono::milliseconds snapshot_save_elapsed{};
    LivePollResult catch_up;
};

// Owns an NTFS catalog and its concurrently searchable metadata index.
// start() first attempts a checksummed metadata snapshot whose embedded journal
// cursor is validated against the current volume. Missing, corrupt or expired
// snapshots fall back to a race-free MFT bootstrap. poll_once() advances the
// session in at-least-once-safe order; save_snapshot() persists a catalog and
// cursor pair while journal mutation is excluded by the session mutex.
class LiveIndexSession {
public:
    LiveIndexSession() = default;
    LiveIndexSession(const LiveIndexSession&) = delete;
    LiveIndexSession& operator=(const LiveIndexSession&) = delete;

    [[nodiscard]] LiveStartResult start(
        const std::filesystem::path& volume,
        const std::filesystem::path& checkpoint_path);
    [[nodiscard]] LivePollResult poll_once();
    [[nodiscard]] LiveSnapshotSaveResult save_snapshot();

    [[nodiscard]] MetadataIndex& index() noexcept { return index_; }
    [[nodiscard]] const MetadataIndex& index() const noexcept { return index_; }
    [[nodiscard]] std::int64_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::uint64_t journal_id() const noexcept {
        return journal_id_;
    }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept {
        return snapshot_path_;
    }

private:
    [[nodiscard]] LivePollResult poll_once_locked();
    [[nodiscard]] LiveSnapshotSaveResult save_snapshot_locked();

    std::filesystem::path volume_;
    std::filesystem::path checkpoint_path_;
    std::filesystem::path snapshot_path_;
    std::filesystem::path wal_path_;
    std::uint64_t root_id_{};
    std::uint64_t journal_id_{};
    std::int64_t cursor_{};
    std::unique_ptr<NtfsCatalog> catalog_;
    MetadataIndex index_;
    mutable std::mutex session_mutex_;
    bool start_attempted_{};
    bool ready_{};
};

[[nodiscard]] const char* live_start_stage_name(LiveStartStage stage) noexcept;
[[nodiscard]] const char* checkpoint_status_name(
    CheckpointStatus status) noexcept;
} // namespace esm

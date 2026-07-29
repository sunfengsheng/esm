#include "esm/live_index.hpp"

#include "esm/journal_replay.hpp"
#include "esm/ntfs_enumerator.hpp"
#include "esm/usn_journal.hpp"

#include <windows.h>

#include <chrono>
#include <new>
#include <utility>

namespace esm {
namespace {
std::filesystem::path normalize_volume_path(
    const std::filesystem::path& volume) {
    auto text = volume.wstring();
    if (text.size() >= 2 && text[1] == L':') {
        wchar_t drive = text[0];
        if (drive >= L'a' && drive <= L'z') drive -= L'a' - L'A';
        return std::filesystem::path(std::wstring(1, drive) + L":");
    }
    return volume;
}

bool same_volume_name(std::wstring_view left,
                      std::wstring_view right) noexcept {
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool wal_file_size(const std::filesystem::path& path,
                   std::uint64_t& bytes,
                   std::uint32_t& error) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            bytes = 0;
            error = ERROR_SUCCESS;
            return true;
        }
        return false;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = ERROR_INVALID_DATA;
        return false;
    }
    bytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) |
            data.nFileSizeLow;
    error = ERROR_SUCCESS;
    return true;
}
} // namespace

const char* live_start_stage_name(LiveStartStage stage) noexcept {
    switch (stage) {
    case LiveStartStage::none: return "none";
    case LiveStartStage::volume_check: return "volume-check";
    case LiveStartStage::journal_query: return "journal-query";
    case LiveStartStage::snapshot_load: return "snapshot-load";
    case LiveStartStage::mft_enumeration: return "mft-enumeration";
    case LiveStartStage::index_build: return "index-build";
    case LiveStartStage::checkpoint_save: return "checkpoint-save";
    case LiveStartStage::catch_up: return "catch-up";
    case LiveStartStage::snapshot_save: return "snapshot-save";
    }
    return "unknown";
}

const char* checkpoint_status_name(CheckpointStatus status) noexcept {
    switch (status) {
    case CheckpointStatus::valid: return "valid";
    case CheckpointStatus::journal_changed: return "journal-changed";
    case CheckpointStatus::expired: return "expired";
    case CheckpointStatus::ahead_of_journal: return "ahead-of-journal";
    }
    return "unknown";
}

LiveStartResult LiveIndexSession::start(
    const std::filesystem::path& volume,
    const std::filesystem::path& checkpoint_path) {
    std::lock_guard session_lock(session_mutex_);
    LiveStartResult result;
    if (start_attempted_) {
        result.stage = LiveStartStage::none;
        result.error = ERROR_ALREADY_INITIALIZED;
        return result;
    }
    start_attempted_ = true;
    volume_ = normalize_volume_path(volume);
    checkpoint_path_ = checkpoint_path;
    snapshot_path_ = metadata_snapshot_path(checkpoint_path_);
    wal_path_ = metadata_wal_path(checkpoint_path_);

    try {
        result.stage = LiveStartStage::volume_check;
        const auto comparison = compare_path_volumes(volume_, checkpoint_path_);
        if (!comparison.ok) {
            result.error = comparison.error;
            return result;
        }
        if (comparison.same_volume) {
            result.checkpoint_same_volume = true;
            result.error = ERROR_INVALID_PARAMETER;
            return result;
        }

        // Query the current journal before accepting a persisted cursor. This
        // prevents stale snapshots from crossing journal recreation or expiry.
        result.stage = LiveStartStage::journal_query;
        const auto bootstrap = query_usn_journal(volume_.wstring());
        if (!bootstrap.available) {
            result.error = bootstrap.error;
            return result;
        }

        result.stage = LiveStartStage::snapshot_load;
        const auto snapshot_started = std::chrono::steady_clock::now();
        auto mapped = load_metadata_snapshot_mapped(snapshot_path_);
        MetadataSnapshotLoadResult legacy;
        const bool legacy_candidate =
            !mapped.ok && mapped.error == ERROR_REVISION_MISMATCH;
        if (legacy_candidate) legacy = load_metadata_snapshot(snapshot_path_);
        result.snapshot_load_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - snapshot_started);

        const bool snapshot_io_ok = mapped.ok || legacy.ok;
        if (snapshot_io_ok) {
            const auto persisted_checkpoint = mapped.ok
                ? mapped.snapshot.checkpoint
                : legacy.snapshot.checkpoint;
            const auto persisted_root_id = mapped.ok
                ? mapped.snapshot.root_id
                : legacy.snapshot.root_id;
            const auto& persisted_volume = mapped.ok
                ? mapped.snapshot.volume
                : legacy.snapshot.volume;
            const auto persisted_entries = mapped.ok
                ? mapped.snapshot.catalog.node_count
                : legacy.snapshot.records.size();

            result.snapshot_checkpoint_status = validate_checkpoint(
                bootstrap, persisted_checkpoint);
            const bool volume_matches = same_volume_name(
                volume_.wstring(), persisted_volume);
            const bool usable = volume_matches &&
                result.snapshot_checkpoint_status == CheckpointStatus::valid &&
                persisted_root_id != 0 && persisted_entries != 0;
            if (usable) {
                result.snapshot_loaded = true;
                journal_id_ = persisted_checkpoint.journal_id;
                cursor_ = persisted_checkpoint.next_usn;
                root_id_ = persisted_root_id;
                result.journal_id = journal_id_;
                result.bootstrap_usn = cursor_;
                result.entries = persisted_entries;

                result.stage = LiveStartStage::index_build;
                const auto index_started = std::chrono::steady_clock::now();
                catalog_ = std::make_unique<NtfsCatalog>(
                    volume_.wstring(), root_id_);
                if (mapped.ok) {
                    if (!catalog_->replace_mapped(
                            std::move(mapped.snapshot.catalog))) {
                        result.error = ERROR_INVALID_DATA;
                        return result;
                    }
                } else {
                    catalog_->replace(legacy.snapshot.records);
                }
                index_.replace(catalog_->snapshot());
                result.index_elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - index_started);
                result.entries = index_.size();

                const auto wal_replay = replay_metadata_wal(
                    wal_path_, journal_id_, cursor_, *catalog_, index_);
                if (!wal_replay.ok) {
                    result.error = wal_replay.error;
                    return result;
                }
                cursor_ = wal_replay.next_usn;
                result.bootstrap_usn = cursor_;
                result.entries = index_.size();

                result.stage = LiveStartStage::checkpoint_save;
                const auto checkpoint_saved = save_checkpoint_atomic(
                    checkpoint_path_, {journal_id_, cursor_});
                if (!checkpoint_saved.ok) {
                    result.error = checkpoint_saved.error;
                    return result;
                }

                result.stage = LiveStartStage::catch_up;
                result.catch_up = poll_once_locked();
                if (!result.catch_up.ok) {
                    result.error = result.catch_up.error;
                    return result;
                }

                // Journal changes require a new state image. A legacy snapshot
                // is also rewritten immediately into the mapped v2 layout.
                if (wal_replay.transactions != 0 ||
                    result.catch_up.batches != 0 || !mapped.ok) {
                    result.stage = LiveStartStage::snapshot_save;
                    const auto saved = save_snapshot_locked();
                    result.snapshot_saved = saved.ok;
                    result.snapshot_save_elapsed = saved.elapsed;
                    if (!saved.ok) {
                        result.error = saved.error;
                        return result;
                    }
                } else {
                    result.snapshot_saved = true;
                }

                ready_ = true;
                result.stage = LiveStartStage::none;
                result.ok = true;
                result.error = ERROR_SUCCESS;
                return result;
            }

            result.snapshot_rejected = true;
            result.snapshot_error = ERROR_INVALID_DATA;
        } else {
            const auto load_error = legacy_candidate ? legacy.error
                                                     : mapped.error;
            result.snapshot_error = load_error;
            result.snapshot_rejected =
                load_error != ERROR_FILE_NOT_FOUND &&
                load_error != ERROR_PATH_NOT_FOUND;
        }

        // Capture the exact Windows-issued USN already returned above, then
        // enumerate the MFT and replay every later change before queries start.
        journal_id_ = bootstrap.journal_id;
        cursor_ = bootstrap.next_usn;
        result.journal_id = journal_id_;
        result.bootstrap_usn = cursor_;

        result.stage = LiveStartStage::mft_enumeration;
        auto scan = enumerate_ntfs_volume(volume_.wstring());
        result.entries = scan.records.size();
        result.scan_errors = scan.errors;
        result.metadata_hydrated = scan.metadata_hydrated;
        result.metadata_errors = scan.metadata_errors;
        result.metadata_elapsed = scan.metadata_elapsed;
        result.enumeration_elapsed =
            scan.elapsed >= scan.metadata_elapsed
                ? scan.elapsed - scan.metadata_elapsed
                : std::chrono::milliseconds{};
        if (scan.root_id == 0 ||
            (scan.records.empty() && scan.errors != 0)) {
            result.error = scan.errors == 0 ? ERROR_INVALID_DATA
                                            : ERROR_READ_FAULT;
            return result;
        }
        root_id_ = scan.root_id;

        catalog_ = std::make_unique<NtfsCatalog>(volume_.wstring(), root_id_);
        catalog_->replace(scan.records);

        result.stage = LiveStartStage::index_build;
        const auto index_started = std::chrono::steady_clock::now();
        index_.replace(std::move(scan.records));
        result.index_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - index_started);
        result.entries = index_.size();

        const auto wal_reset = reset_metadata_wal(wal_path_);
        if (!wal_reset.ok) {
            result.error = wal_reset.error;
            return result;
        }

        result.stage = LiveStartStage::checkpoint_save;
        const auto checkpoint_saved = save_checkpoint_atomic(
            checkpoint_path_, {journal_id_, cursor_});
        if (!checkpoint_saved.ok) {
            result.error = checkpoint_saved.error;
            return result;
        }

        result.stage = LiveStartStage::catch_up;
        result.catch_up = poll_once_locked();
        if (!result.catch_up.ok) {
            result.error = result.catch_up.error;
            return result;
        }

        result.stage = LiveStartStage::snapshot_save;
        const auto snapshot_saved = save_snapshot_locked();
        result.snapshot_saved = snapshot_saved.ok;
        result.snapshot_save_elapsed = snapshot_saved.elapsed;
        if (!snapshot_saved.ok) {
            result.error = snapshot_saved.error;
            return result;
        }

        ready_ = true;
        result.stage = LiveStartStage::none;
        result.ok = true;
        result.error = ERROR_SUCCESS;
        return result;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
        return result;
    }
}

LivePollResult LiveIndexSession::poll_once() {
    std::lock_guard session_lock(session_mutex_);
    return poll_once_locked();
}

LivePollResult LiveIndexSession::poll_once_locked() {
    LivePollResult result;
    result.next_usn = cursor_;
    if (!catalog_ || journal_id_ == 0) {
        result.error = ERROR_INVALID_STATE;
        return result;
    }

    try {
        const auto state = query_usn_journal(volume_.wstring());
        if (!state.available) {
            result.error = state.error;
            return result;
        }

        result.checkpoint_status = validate_checkpoint(
            state, {journal_id_, cursor_});
        if (result.checkpoint_status != CheckpointStatus::valid) {
            result.rebuild_required = true;
            result.error = ERROR_INVALID_DATA;
            return result;
        }

        const auto target = state.next_usn;
        while (cursor_ < target) {
            const auto batch = read_usn_changes(
                volume_.wstring(), state, cursor_);
            if (batch.error != ERROR_SUCCESS) {
                result.error = batch.error;
                return result;
            }
            if (batch.next_usn <= cursor_) {
                result.error = ERROR_INVALID_DATA;
                return result;
            }

            const auto replay = replay_journal_batch(
                *catalog_, index_, state, batch, checkpoint_path_, wal_path_, cursor_);
            if (replay.error != ERROR_SUCCESS) {
                result.error = replay.error;
                return result;
            }

            cursor_ = replay.next_usn;
            result.next_usn = cursor_;
            ++result.batches;
            result.changes += batch.changes.size();
            result.created += replay.created;
            result.updated += replay.updated;
            result.renamed += replay.renamed;
            result.deleted += replay.deleted;
            result.ignored += replay.ignored;
        }
        if (!wal_file_size(wal_path_, result.wal_bytes, result.error)) {
            return result;
        }
        if (ready_ && result.wal_bytes >= default_wal_checkpoint_bytes) {
            const auto consolidated = save_snapshot_locked();
            if (!consolidated.ok) {
                result.error = consolidated.error;
                return result;
            }
            result.checkpoint_consolidated = true;
            result.checkpoint_elapsed = consolidated.elapsed;
            result.wal_bytes = 0;
        }
        result.ok = true;
        result.error = ERROR_SUCCESS;
        result.next_usn = cursor_;
        return result;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
        return result;
    }
}

LiveSnapshotSaveResult LiveIndexSession::save_snapshot() {
    std::lock_guard session_lock(session_mutex_);
    return save_snapshot_locked();
}

LiveSnapshotSaveResult LiveIndexSession::save_snapshot_locked() {
    LiveSnapshotSaveResult result;
    if (!catalog_ || root_id_ == 0 || journal_id_ == 0 ||
        snapshot_path_.empty()) {
        result.error = ERROR_INVALID_STATE;
        return result;
    }

    try {
        const auto started = std::chrono::steady_clock::now();
        if (catalog_->pending_delta_size() != 0) {
            (void)catalog_->compact();
        } else {
            (void)catalog_->materialize_mapped_base();
        }
        result.entries = catalog_->size();
        const auto saved = save_metadata_catalog_snapshot_atomic(
            snapshot_path_, {journal_id_, cursor_}, root_id_,
            volume_.wstring(), *catalog_);
        if (!saved.ok) {
            result.error = saved.error;
        } else {
            const auto checkpoint_saved = save_checkpoint_atomic(
                checkpoint_path_, {journal_id_, cursor_});
            if (!checkpoint_saved.ok) {
                result.error = checkpoint_saved.error;
            } else {
                const auto wal_reset = reset_metadata_wal(wal_path_);
                result.ok = wal_reset.ok;
                result.error = wal_reset.error;
            }
        }
        result.elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        return result;
    } catch (const std::bad_alloc&) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
        return result;
    } catch (...) {
        result.error = ERROR_UNHANDLED_EXCEPTION;
        return result;
    }
}
} // namespace esm

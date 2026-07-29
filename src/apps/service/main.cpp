#include "esm/index.hpp"
#include "esm/file_metadata.hpp"
#include "esm/live_index.hpp"
#include "esm/journal_checkpoint.hpp"
#include "esm/metadata_snapshot.hpp"
#include "esm/metadata_hydration_wal.hpp"
#include "esm/metadata_wal.hpp"
#include "esm/mft_auto_state.hpp"
#include "esm/named_pipe.hpp"
#include "esm/ntfs_enumerator.hpp"
#include "esm/usn_journal.hpp"
#include "esm/volume_discovery.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t service_name[] = L"everything_sm";
constexpr wchar_t service_display_name[] = L"everything_sm Search Service";
constexpr wchar_t default_pipe_name[] = L"everything_sm";
constexpr std::uint64_t slow_query_threshold_microseconds = 100'000;
constexpr auto slow_query_log_interval = std::chrono::seconds(5);
constexpr DWORD service_wait_timeout_ms = 120'000;
constexpr auto snapshot_refresh_interval = std::chrono::minutes(5);
constexpr std::size_t snapshot_refresh_changes = 100'000;
constexpr auto mft_reconciliation_interval = std::chrono::minutes(1);
constexpr auto mft_live_poll_interval = std::chrono::milliseconds(250);
constexpr auto metadata_hydration_poll_interval = std::chrono::milliseconds(10);
constexpr std::size_t metadata_hydration_batch_size = 4'096;
constexpr std::size_t metadata_hydration_progress_interval = 250'000;
constexpr auto mft_delta_checkpoint_interval = std::chrono::minutes(5);
constexpr std::size_t mft_delta_checkpoint_changes = 100'000;
constexpr wchar_t mft_auto_snapshot_name[] = L"mft-index.snapshot";
constexpr wchar_t mft_auto_snapshot_marker[] =
    L"everything_sm-mft-multi-v1";

struct ServiceHandle {
    SC_HANDLE value{};
    ~ServiceHandle() { if (value != nullptr) CloseServiceHandle(value); }
};

struct ServiceRuntime {
    SERVICE_STATUS_HANDLE handle{};
    SERVICE_STATUS status{};
    std::mutex status_mutex;
    std::atomic_bool stop{};
    std::atomic<DWORD> state{SERVICE_STOPPED};
    DWORD checkpoint{};
};

enum class ServiceMode {
    live,
    mft,
    mft_auto,
};

struct ServiceConfiguration {
    ServiceMode mode{ServiceMode::live};
    std::filesystem::path volume;
    std::filesystem::path checkpoint;
    std::filesystem::path data_directory;
    std::wstring pipe_name;
};

ServiceRuntime runtime;
ServiceConfiguration configuration;

void release_transient_process_memory() {
    // Full MFT reconciliation and snapshot generation temporarily allocate
    // multi-million-record vectors alongside the live search index. _heapmin
    // asks the CRT allocator to decommit completely free regions, reducing
    // Private Bytes rather than merely trimming the process working set.
    (void)_heapmin();
    if (const auto heap = GetProcessHeap(); heap != nullptr) {
        (void)HeapCompact(heap, 0);
    }
}

void log_event(WORD type, std::wstring_view message) {
    HANDLE source = RegisterEventSourceW(nullptr, service_name);
    if (source == nullptr) return;
    std::wstring stable(message);
    LPCWSTR strings[] = {stable.c_str()};
    ReportEventW(source, type, 0, 1, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}

void log_pipe_search_diagnostics(
    const esm::PipeSearchDiagnostics& diagnostics) {
    if (diagnostics.error != ERROR_SUCCESS ||
        diagnostics.total_microseconds < slow_query_threshold_microseconds) {
        return;
    }

    static std::mutex mutex;
    static auto last_logged = std::chrono::steady_clock::time_point{};
    static std::size_t suppressed{};
    std::wstring message;
    {
        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        if (last_logged != std::chrono::steady_clock::time_point{} &&
            now - last_logged < slow_query_log_interval) {
            ++suppressed;
            return;
        }
        message = esm::format_pipe_search_diagnostics(diagnostics);
        if (suppressed != 0) {
            message += L", suppressed_since_last=" +
                       std::to_wstring(suppressed);
            suppressed = 0;
        }
        last_logged = now;
    }
    log_event(EVENTLOG_WARNING_TYPE, message);
}

void report_service_status(DWORD state, DWORD win32_error = ERROR_SUCCESS,
                           DWORD wait_hint = 0,
                           DWORD service_specific_error = 0) {
    std::lock_guard lock(runtime.status_mutex);
    runtime.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    runtime.status.dwCurrentState = state;
    runtime.status.dwWin32ExitCode = win32_error;
    runtime.status.dwServiceSpecificExitCode = service_specific_error;
    runtime.status.dwWaitHint = wait_hint;
    runtime.status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
        runtime.status.dwCheckPoint = ++runtime.checkpoint;
    else {
        runtime.checkpoint = 0;
        runtime.status.dwCheckPoint = 0;
    }
    runtime.state.store(state, std::memory_order_relaxed);
    if (runtime.handle != nullptr)
        SetServiceStatus(runtime.handle, &runtime.status);
}

DWORD WINAPI service_control_handler(DWORD control, DWORD, void*, void*) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN: {
        const auto state = runtime.state.load(std::memory_order_relaxed);
        if (state == SERVICE_RUNNING || state == SERVICE_START_PENDING) {
            runtime.stop.store(true, std::memory_order_relaxed);
            report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 30'000);
        }
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE: {
        std::lock_guard lock(runtime.status_mutex);
        if (runtime.handle != nullptr)
            SetServiceStatus(runtime.handle, &runtime.status);
        return NO_ERROR;
    }
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

std::wstring service_start_failure(const esm::LiveStartResult& result) {
    if (result.checkpoint_same_volume)
        return L"Checkpoint path is on the indexed volume";
    std::wstring message = L"Live index start failed at ";
    const auto* stage = esm::live_start_stage_name(result.stage);
    while (*stage != '\0') message.push_back(static_cast<wchar_t>(*stage++));
    message += L", error=" + std::to_wstring(result.error);
    return message;
}

struct VolumeLiveState {
    esm::NtfsVolumeInfo volume;
    std::uint64_t root_id{};
    std::uint64_t journal_id{};
    std::int64_t cursor{};
    bool live{};
};

struct VolumeBuildResult {
    VolumeLiveState state;
    std::vector<esm::FileRecord> records;
    DWORD error{ERROR_SUCCESS};
};

struct MultiVolumeBuildResult {
    std::vector<VolumeLiveState> states;
    std::vector<esm::FileRecord> records;
    std::vector<std::wstring> volumes;
    std::size_t failures{};
    DWORD error{ERROR_SUCCESS};
};

struct BackgroundMetadataHydration {
    bool active{};
    std::uint64_t after_id{};
    std::size_t examined{};
    std::size_t attempted{};
    std::size_t hydrated{};
    std::size_t errors{};
    std::size_t applied{};
    std::size_t stale{};
    std::uint64_t generation{};
    std::filesystem::path wal_path;
    bool wal_enabled{};
    bool wal_warning_logged{};
    std::size_t next_progress_report{metadata_hydration_progress_interval};
    std::chrono::steady_clock::time_point started{};
};

std::wstring join_volumes(const std::vector<std::wstring>& volumes) {
    std::wstring result;
    for (const auto& volume : volumes) {
        if (!result.empty()) result += L", ";
        result += volume;
    }
    return result.empty() ? L"(none)" : result;
}

bool same_mounted_volumes(
    const std::vector<VolumeLiveState>& states,
    const std::vector<esm::NtfsVolumeInfo>& discovered) {
    if (states.size() != discovered.size()) return false;
    for (const auto& volume : discovered) {
        const auto found = std::find_if(
            states.begin(), states.end(), [&](const VolumeLiveState& state) {
                return state.volume.identity == volume.identity;
            });
        if (found == states.end()) return false;
    }
    return true;
}

esm::MetadataWalBinding metadata_wal_binding(
    const VolumeLiveState& state, std::uint64_t generation) {
    esm::MetadataWalBinding binding;
    binding.generation = generation;
    binding.volume_identity = state.volume.identity;
    binding.volume_root = state.volume.root;
    binding.root_id = state.root_id;
    return binding;
}

std::filesystem::path volume_metadata_wal_path(
    const std::filesystem::path& snapshot_path,
    const VolumeLiveState& state,
    std::uint64_t generation) {
    return esm::metadata_wal_path(snapshot_path, generation,
                                  state.volume.identity);
}

DWORD catch_up_volume(VolumeLiveState& state,
                      esm::MetadataIndex& index,
                      const std::filesystem::path& snapshot_path,
                      std::uint64_t generation,
                      std::size_t* applied_changes = nullptr) {
    if (!state.live) return ERROR_NOT_SUPPORTED;
    const auto journal = esm::query_usn_journal(state.volume.root);
    if (!journal.available) return journal.error;
    if (esm::validate_checkpoint(
            journal, {state.journal_id, state.cursor}) !=
        esm::CheckpointStatus::valid) {
        return ERROR_INVALID_DATA;
    }

    const auto target = journal.next_usn;
    while (state.cursor < target) {
        const auto batch = esm::read_usn_changes(
            state.volume.root, journal, state.cursor);
        if (batch.error != ERROR_SUCCESS) return batch.error;
        if (batch.next_usn <= state.cursor) return ERROR_INVALID_DATA;

        const auto appended = esm::append_metadata_wal(
            volume_metadata_wal_path(snapshot_path, state, generation),
            metadata_wal_binding(state, generation), state.journal_id,
            state.cursor, batch);
        if (!appended.ok) return appended.error;

        index.apply_ntfs_changes(state.volume.identity,
                                 state.volume.root,
                                 state.root_id,
                                 batch);
        state.cursor = batch.next_usn;
        if (applied_changes != nullptr) {
            *applied_changes += batch.changes.size();
        }
    }
    return ERROR_SUCCESS;
}

DWORD catch_up_metadata_reuse_sources(
    std::vector<VolumeLiveState>& previous,
    const std::vector<VolumeLiveState>& rebuilt,
    esm::MetadataIndex& index,
    const std::filesystem::path& snapshot_path,
    std::uint64_t generation,
    std::wstring& failed_volume) {
    for (auto& state : previous) {
        const auto current = std::find_if(
            rebuilt.begin(), rebuilt.end(), [&](const VolumeLiveState& item) {
                return item.volume.identity == state.volume.identity;
            });
        if (current == rebuilt.end()) continue;
        if (!state.live || !current->live) {
            failed_volume = state.volume.root;
            return ERROR_NOT_SUPPORTED;
        }
        const auto error = catch_up_volume(
            state, index, snapshot_path, generation);
        if (error != ERROR_SUCCESS) {
            failed_volume = state.volume.root;
            return error;
        }
    }
    return ERROR_SUCCESS;
}

VolumeBuildResult build_volume_live_state(esm::NtfsVolumeInfo volume) {
    VolumeBuildResult result;
    result.state.volume = std::move(volume);

    const auto boundary =
        esm::query_usn_journal(result.state.volume.root);
    auto scan = esm::enumerate_ntfs_volume(
        result.state.volume.root,
        {.hydrate_search_metadata = false});
    if (scan.records.empty() && scan.errors != 0) {
        result.error = ERROR_READ_FAULT;
        return result;
    }

    result.state.root_id = scan.root_id;
    if (boundary.available) {
        result.state.journal_id = boundary.journal_id;
        result.state.cursor = boundary.next_usn;
        result.state.live = true;
    }

    result.records = std::move(scan.records);
    esm::namespace_ntfs_records(result.state.volume.identity,
                                result.records);
    return result;
}

MultiVolumeBuildResult build_all_ntfs_volumes() {
    MultiVolumeBuildResult result;
    const auto discovery = esm::discover_mounted_ntfs_volumes();
    if (discovery.error != ERROR_SUCCESS) {
        result.error = discovery.error;
        return result;
    }
    if (discovery.volumes.empty()) {
        result.error = ERROR_NOT_FOUND;
        return result;
    }

    struct PendingBuild {
        std::wstring root;
        std::future<VolumeBuildResult> future;
    };
    std::vector<PendingBuild> pending;
    pending.reserve(discovery.volumes.size());
    for (auto volume : discovery.volumes) {
        const auto root = volume.root;
        pending.push_back({
            root,
            std::async(std::launch::async,
                       [volume = std::move(volume)]() mutable {
                           return build_volume_live_state(
                               std::move(volume));
                       })});
    }

    // Collect completed volume vectors first so the combined vector can reserve
    // its exact final size. Appending C:, D:, E: directly caused several
    // hundred MiB reallocations; the freed intermediate buffers fragmented the
    // long-lived service heap and made Private Bytes grow after each periodic
    // reconciliation.
    std::vector<VolumeBuildResult> completed;
    completed.reserve(pending.size());
    std::size_t total_records = 0;
    for (auto& item : pending) {
        try {
            auto built = item.future.get();
            if (built.error != ERROR_SUCCESS || built.records.empty()) {
                ++result.failures;
                log_event(EVENTLOG_WARNING_TYPE,
                          L"MFT enumeration failed for " + item.root +
                              L"; keeping the previous complete index");
                continue;
            }
            if (built.records.size() >
                std::numeric_limits<std::size_t>::max() - total_records) {
                result.error = ERROR_ARITHMETIC_OVERFLOW;
                ++result.failures;
                continue;
            }
            total_records += built.records.size();
            completed.push_back(std::move(built));
        } catch (const std::exception&) {
            ++result.failures;
            log_event(EVENTLOG_WARNING_TYPE,
                      L"MFT enumeration raised an exception for " +
                          item.root);
        }
    }

    result.records.reserve(total_records);
    result.volumes.reserve(completed.size());
    result.states.reserve(completed.size());
    for (auto& built : completed) {
        result.volumes.push_back(built.state.volume.root);
        result.records.insert(
            result.records.end(),
            std::make_move_iterator(built.records.begin()),
            std::make_move_iterator(built.records.end()));
        // Move-insert leaves the source vector's FileRecord storage allocated.
        // Release each per-volume buffer before merging the next one so the
        // exact-size destination does not coexist with all source buffers.
        std::vector<esm::FileRecord>().swap(built.records);
        result.states.push_back(std::move(built.state));
    }
    if (result.volumes.empty() && result.error == ERROR_SUCCESS) {
        result.error = ERROR_READ_FAULT;
    }
    return result;
}

std::uint64_t create_snapshot_generation() noexcept {
    FILETIME current_time{};
    GetSystemTimeAsFileTime(&current_time);
    ULARGE_INTEGER timestamp{};
    timestamp.LowPart = current_time.dwLowDateTime;
    timestamp.HighPart = current_time.dwHighDateTime;
    LARGE_INTEGER performance_counter{};
    (void)QueryPerformanceCounter(&performance_counter);
    static std::atomic_uint64_t sequence{1};
    auto generation = timestamp.QuadPart ^
        static_cast<std::uint64_t>(performance_counter.QuadPart) ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) ^
        sequence.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0) generation = 1;
    return generation;
}

bool ordinal_equal_case_insensitive(std::wstring_view left,
                                    std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()), right.data(),
               static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

esm::MftAutoState make_mft_auto_state(
    std::uint64_t generation,
    const std::vector<VolumeLiveState>& states) {
    esm::MftAutoState result;
    result.generation = generation;
    result.volumes.reserve(states.size());
    for (const auto& state : states) {
        esm::MftAutoVolumeState saved;
        saved.volume = state.volume;
        saved.root_id = state.root_id;
        saved.journal_id = state.journal_id;
        saved.cursor = state.cursor;
        saved.live = state.live;
        result.volumes.push_back(std::move(saved));
    }
    return result;
}

struct MftAutoRestoreResult {
    bool ok{};
    DWORD error{ERROR_SUCCESS};
    std::vector<VolumeLiveState> states;
};

MftAutoRestoreResult restore_mft_auto_states(
    const std::filesystem::path& path,
    std::uint64_t generation) {
    MftAutoRestoreResult result;
    const auto loaded = esm::load_mft_auto_state(path, generation);
    if (!loaded.ok) {
        result.error = loaded.error;
        return result;
    }
    const auto discovered = esm::discover_mounted_ntfs_volumes();
    if (discovered.error != ERROR_SUCCESS) {
        result.error = discovered.error;
        return result;
    }
    if (loaded.state.volumes.size() != discovered.volumes.size()) {
        result.error = ERROR_INVALID_DATA;
        return result;
    }

    result.states.reserve(loaded.state.volumes.size());
    for (const auto& saved : loaded.state.volumes) {
        const auto current = std::find_if(
            discovered.volumes.begin(), discovered.volumes.end(),
            [&](const esm::NtfsVolumeInfo& volume) {
                return ordinal_equal_case_insensitive(
                    saved.volume.identity, volume.identity);
            });
        if (current == discovered.volumes.end() || !saved.live ||
            !ordinal_equal_case_insensitive(saved.volume.root,
                                            current->root) ||
            saved.volume.serial_number != current->serial_number) {
            result.error = ERROR_INVALID_DATA;
            result.states.clear();
            return result;
        }
        const auto current_root_id =
            esm::query_ntfs_root_file_id(current->root);
        if (current_root_id == 0 || saved.root_id == 0 ||
            current_root_id != saved.root_id) {
            result.error = ERROR_INVALID_DATA;
            result.states.clear();
            return result;
        }
        const auto journal = esm::query_usn_journal(current->root);
        if (!journal.available ||
            esm::validate_checkpoint(
                journal, {saved.journal_id, saved.cursor}) !=
                esm::CheckpointStatus::valid) {
            result.error = journal.available ? ERROR_INVALID_DATA
                                             : journal.error;
            result.states.clear();
            return result;
        }

        VolumeLiveState restored;
        restored.volume = *current;
        restored.root_id = current_root_id;
        restored.journal_id = saved.journal_id;
        restored.cursor = saved.cursor;
        restored.live = true;
        result.states.push_back(std::move(restored));
    }
    result.ok = true;
    result.error = ERROR_SUCCESS;
    return result;
}

struct MftAutoWalReplayResult {
    bool ok{};
    DWORD error{ERROR_SUCCESS};
    std::size_t transactions{};
    std::size_t changes{};
    std::uint64_t discarded_tail_bytes{};
};

MftAutoWalReplayResult replay_mft_auto_metadata_wals(
    const std::filesystem::path& snapshot_path,
    std::uint64_t generation,
    std::vector<VolumeLiveState>& states,
    esm::MetadataIndex& index) {
    MftAutoWalReplayResult result;
    for (auto& state : states) {
        const auto replay = esm::replay_metadata_wal(
            volume_metadata_wal_path(snapshot_path, state, generation),
            metadata_wal_binding(state, generation), state.journal_id,
            state.cursor, index);
        if (!replay.ok) {
            result.error = replay.error;
            return result;
        }
        const auto journal = esm::query_usn_journal(state.volume.root);
        if (!journal.available || replay.next_usn > journal.next_usn ||
            esm::validate_checkpoint(
                journal, {state.journal_id, replay.next_usn}) !=
                esm::CheckpointStatus::valid) {
            result.error = journal.available ? ERROR_INVALID_DATA
                                             : journal.error;
            return result;
        }
        state.cursor = replay.next_usn;
        result.transactions += replay.transactions;
        result.changes += replay.changes;
        result.discarded_tail_bytes += replay.discarded_tail_bytes;
    }
    result.ok = true;
    return result;
}

void remove_generation_metadata_wals(
    const std::filesystem::path& snapshot_path,
    std::uint64_t generation,
    const std::vector<VolumeLiveState>& states) {
    if (generation == 0) return;
    for (const auto& state : states) {
        std::error_code ignored;
        std::filesystem::remove(
            volume_metadata_wal_path(snapshot_path, state, generation),
            ignored);
    }
}

void start_background_metadata_hydration(
    BackgroundMetadataHydration& state,
    std::uint64_t generation,
    const std::filesystem::path& wal_path,
    bool wal_enabled,
    std::uint64_t after_id = 0) {
    state = {};
    state.active = true;
    state.after_id = after_id;
    state.generation = generation;
    state.wal_path = wal_path;
    state.wal_enabled = wal_enabled;
    state.next_progress_report = metadata_hydration_progress_interval;
    state.started = std::chrono::steady_clock::now();
}

std::wstring background_metadata_hydration_summary(
    std::wstring_view prefix,
    const BackgroundMetadataHydration& state) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - state.started);
    return std::wstring(prefix) + L": examined=" +
           std::to_wstring(state.examined) + L", attempted=" +
           std::to_wstring(state.attempted) + L", hydrated=" +
           std::to_wstring(state.hydrated) + L", errors=" +
           std::to_wstring(state.errors) + L", applied=" +
           std::to_wstring(state.applied) + L", stale=" +
           std::to_wstring(state.stale) + L", WAL=" +
           (state.wal_enabled ? L"enabled" : L"disabled") + L", elapsed=" +
           std::to_wstring(elapsed.count()) + L" ms";
}

void run_background_metadata_hydration_batch(
    esm::MetadataIndex& index,
    BackgroundMetadataHydration& state) {
    if (!state.active) return;

    const auto before_id = state.after_id;
    auto batch = index.metadata_hydration_batch(
        before_id, metadata_hydration_batch_size);
    state.examined += batch.examined;

    std::vector<std::uint8_t> succeeded(batch.records.size());
    std::vector<esm::MetadataHydrationWalUpdate> updates;
    if (!batch.records.empty()) {
        const auto hydrated = esm::hydrate_file_search_metadata_records(
            batch.records, 0, succeeded);
        state.attempted += hydrated.attempted;
        state.hydrated += hydrated.hydrated;
        state.errors += hydrated.errors;

        updates.reserve(hydrated.hydrated);
        std::size_t write = 0;
        for (std::size_t index = 0; index < batch.records.size(); ++index) {
            if (succeeded[index] == 0) continue;
            updates.push_back(
                esm::make_metadata_hydration_wal_update(
                    batch.records[index]));
            if (write != index) {
                batch.records[write] = std::move(batch.records[index]);
            }
            ++write;
        }
        batch.records.resize(write);
    }

    if (state.wal_enabled) {
        const auto appended = esm::append_metadata_hydration_wal(
            state.wal_path, state.generation, before_id, batch.next_id,
            batch.examined, updates, batch.complete);
        if (!appended.ok) {
            state.wal_enabled = false;
            if (!state.wal_warning_logged) {
                state.wal_warning_logged = true;
                log_event(
                    EVENTLOG_WARNING_TYPE,
                    L"Metadata hydration WAL append failed, error=" +
                        std::to_wstring(appended.error) +
                        L"; continuing in memory until the next snapshot");
            }
        }
    }

    if (!batch.records.empty()) {
        const auto applied = index.apply_search_metadata(batch.records);
        state.applied += applied.applied;
        state.stale += applied.stale;
    }
    state.after_id = batch.next_id;

    if (batch.complete) {
        log_event(EVENTLOG_INFORMATION_TYPE,
                  background_metadata_hydration_summary(
                      L"Background metadata hydration completed", state));
        state.active = false;
        return;
    }

    if (state.examined >= state.next_progress_report) {
        log_event(EVENTLOG_INFORMATION_TYPE,
                  background_metadata_hydration_summary(
                      L"Background metadata hydration progress", state));
        while (state.next_progress_report <= state.examined) {
            state.next_progress_report += metadata_hydration_progress_interval;
        }
    }
}

bool valid_mft_auto_snapshot(const esm::MetadataSnapshot& snapshot) {
    return snapshot.volume == mft_auto_snapshot_marker &&
           !snapshot.records.empty();
}

esm::MetadataSnapshotIoResult save_mft_auto_snapshot(
    const std::filesystem::path& path,
    std::vector<esm::FileRecord>& records,
    std::uint64_t generation) {
    esm::MetadataSnapshot snapshot;
    snapshot.checkpoint.journal_id = generation;
    snapshot.root_id = 1;
    snapshot.volume = mft_auto_snapshot_marker;
    snapshot.records.swap(records);
    const auto result = esm::save_metadata_snapshot_atomic(path, snapshot);
    snapshot.records.swap(records);
    return result;
}

struct MftAutoPersistenceResult {
    esm::MetadataSnapshotIoResult snapshot;
    esm::MetadataHydrationWalResult hydration_wal;
    esm::MftAutoStateIoResult state;
    bool metadata_wals_ok{};
    DWORD metadata_wal_error{ERROR_SUCCESS};

    [[nodiscard]] bool committed() const noexcept {
        return snapshot.ok && metadata_wals_ok && state.ok;
    }
};

MftAutoPersistenceResult persist_mft_auto_generation(
    const std::filesystem::path& snapshot_path,
    std::vector<esm::FileRecord>& records,
    const std::vector<VolumeLiveState>& states,
    std::uint64_t generation) {
    MftAutoPersistenceResult result;
    result.snapshot = save_mft_auto_snapshot(
        snapshot_path, records, generation);
    if (!result.snapshot.ok) return result;

    result.hydration_wal = esm::initialize_metadata_hydration_wal(
        esm::metadata_hydration_wal_path(snapshot_path), generation);
    result.metadata_wals_ok = true;
    for (const auto& state : states) {
        const auto reset = esm::reset_metadata_wal(
            volume_metadata_wal_path(snapshot_path, state, generation));
        if (!reset.ok) {
            result.metadata_wals_ok = false;
            result.metadata_wal_error = reset.error;
            break;
        }
    }
    if (!result.metadata_wals_ok) return result;
    result.state = esm::save_mft_auto_state_atomic(
        esm::mft_auto_state_path(snapshot_path),
        make_mft_auto_state(generation, states));
    return result;
}


void run_mft_auto_service() {
    std::error_code directory_error;
    std::filesystem::create_directories(configuration.data_directory,
                                        directory_error);
    if (directory_error) {
        const DWORD error = static_cast<DWORD>(directory_error.value());
        log_event(EVENTLOG_ERROR_TYPE,
                  L"Could not create MFT index directory: " +
                      configuration.data_directory.wstring() + L", error=" +
                      std::to_wstring(error));
        report_service_status(SERVICE_STOPPED, error, 0, error);
        return;
    }

    const auto snapshot_path =
        configuration.data_directory / mft_auto_snapshot_name;
    const auto hydration_wal_path =
        esm::metadata_hydration_wal_path(snapshot_path);
    const auto state_path = esm::mft_auto_state_path(snapshot_path);
    esm::MetadataIndex index;
    bool loaded_snapshot = false;
    bool startup_rebuild_required = true;
    bool initial_hydration_scheduled = false;
    bool initial_hydration_wal_ready = false;
    bool initial_hydration_complete = false;
    std::uint64_t initial_hydration_after_id = 0;
    std::uint64_t current_generation = 0;
    std::vector<VolumeLiveState> initial_states;

    auto loaded = esm::load_metadata_snapshot(snapshot_path);
    if (loaded.ok && valid_mft_auto_snapshot(loaded.snapshot)) {
        const auto count = loaded.snapshot.records.size();
        current_generation = loaded.snapshot.checkpoint.journal_id;
        if (current_generation != 0) {
            const auto replay = esm::replay_metadata_hydration_wal(
                hydration_wal_path, current_generation,
                loaded.snapshot.records);
            if (replay.ok) {
                initial_hydration_wal_ready = true;
                initial_hydration_complete = replay.complete;
                initial_hydration_after_id = replay.next_id;
                log_event(
                    EVENTLOG_INFORMATION_TYPE,
                    L"Replayed metadata hydration WAL: transactions=" +
                        std::to_wstring(replay.transactions) + L", updates=" +
                        std::to_wstring(replay.updates) + L", applied=" +
                        std::to_wstring(replay.applied) + L", stale=" +
                        std::to_wstring(replay.stale) + L", next ID=" +
                        std::to_wstring(replay.next_id));
                if (replay.torn_tail) {
                    log_event(
                        EVENTLOG_WARNING_TYPE,
                        L"Recovered a torn metadata hydration WAL tail; "
                        L"discarded bytes=" +
                            std::to_wstring(replay.discarded_tail_bytes));
                }
            } else {
                if (std::filesystem::exists(hydration_wal_path)) {
                    log_event(
                        EVENTLOG_WARNING_TYPE,
                        L"Metadata hydration WAL could not be replayed, "
                        L"error=" + std::to_wstring(replay.error) +
                            L"; restarting metadata hydration from ID 0");
                }
                const auto initialized =
                    esm::initialize_metadata_hydration_wal(
                        hydration_wal_path, current_generation);
                initial_hydration_wal_ready = initialized.ok;
                if (!initialized.ok) {
                    log_event(
                        EVENTLOG_WARNING_TYPE,
                        L"Metadata hydration WAL reset failed, error=" +
                            std::to_wstring(initialized.error) +
                            L"; metadata will be restored in memory only");
                }
            }

            auto restored = restore_mft_auto_states(
                state_path, current_generation);
            if (restored.ok) {
                initial_states = std::move(restored.states);
                startup_rebuild_required = false;
                log_event(
                    EVENTLOG_INFORMATION_TYPE,
                    L"Validated multi-volume snapshot state and USN "
                    L"boundaries for " +
                        std::to_wstring(initial_states.size()) + L" volumes");
            } else {
                log_event(
                    EVENTLOG_WARNING_TYPE,
                    L"Multi-volume snapshot state is unavailable or stale, "
                    L"error=" + std::to_wstring(restored.error) +
                        L"; scheduling an MFT reconciliation");
            }
        } else {
            log_event(
                EVENTLOG_INFORMATION_TYPE,
                L"Loaded a legacy multi-volume snapshot without a generation; "
                L"scheduling an MFT reconciliation");
        }

        index.replace(std::move(loaded.snapshot.records));
        loaded_snapshot = true;
        if (!startup_rebuild_required) {
            const auto replayed = replay_mft_auto_metadata_wals(
                snapshot_path, current_generation, initial_states, index);
            if (replayed.ok) {
                if (replayed.changes != 0) {
                    initial_hydration_complete = false;
                    initial_hydration_after_id = 0;
                }
                log_event(
                    EVENTLOG_INFORMATION_TYPE,
                    L"Replayed multi-volume name/USN WALs: transactions=" +
                        std::to_wstring(replayed.transactions) + L", changes=" +
                        std::to_wstring(replayed.changes));
                if (replayed.discarded_tail_bytes != 0) {
                    log_event(
                        EVENTLOG_WARNING_TYPE,
                        L"Recovered torn multi-volume name/USN WAL tails; "
                        L"discarded bytes=" +
                            std::to_wstring(replayed.discarded_tail_bytes));
                }
            } else {
                // replay_mft_auto_metadata_wals applies one volume at a time.
                // If a later volume fails validation, discard every partial
                // replay before publishing queries. Reloading the immutable
                // checkpoint is cheaper than retaining a second multi-million
                // record vector solely for rollback.
                startup_rebuild_required = true;
                initial_states.clear();
                const auto fallback = esm::load_metadata_snapshot(snapshot_path);
                if (fallback.ok && valid_mft_auto_snapshot(fallback.snapshot) &&
                    fallback.snapshot.checkpoint.journal_id ==
                        current_generation) {
                    index.replace(std::move(fallback.snapshot.records));
                    initial_hydration_complete = false;
                    initial_hydration_after_id = 0;
                    log_event(
                        EVENTLOG_WARNING_TYPE,
                        L"Multi-volume name/USN WAL replay failed, error=" +
                            std::to_wstring(replayed.error) +
                            L"; rolled back partial replay to the durable "
                            L"checkpoint and scheduled reconciliation");
                } else {
                    loaded_snapshot = false;
                    current_generation = 0;
                    log_event(
                        EVENTLOG_ERROR_TYPE,
                        L"Multi-volume name/USN WAL replay failed and the "
                        L"durable checkpoint could not be reloaded; performing "
                        L"a synchronous MFT rebuild");
                }
            }
        }
        initial_hydration_scheduled = loaded_snapshot &&
            !startup_rebuild_required && !initial_hydration_complete;
        if (loaded_snapshot) {
            log_event(EVENTLOG_INFORMATION_TYPE,
                      L"Loaded " + std::to_wstring(count) +
                          L" entries from the multi-volume MFT snapshot");
        }
    } else if (loaded.ok) {
        log_event(EVENTLOG_WARNING_TYPE,
                  L"Ignoring an incompatible multi-volume MFT snapshot");
    } else if (std::filesystem::exists(snapshot_path)) {
        log_event(EVENTLOG_WARNING_TYPE,
                  L"Could not load the multi-volume MFT snapshot, error=" +
                      std::to_wstring(loaded.error));
    }

    if (!loaded_snapshot) {
        auto startup = std::async(std::launch::async,
                                  build_all_ntfs_volumes);
        while (startup.wait_for(std::chrono::seconds(1)) !=
               std::future_status::ready) {
            if (runtime.stop.load(std::memory_order_relaxed))
                report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS,
                                      300'000);
            else
                report_service_status(SERVICE_START_PENDING, ERROR_SUCCESS,
                                      300'000);
        }
        auto built = startup.get();
        if (runtime.stop.load(std::memory_order_relaxed)) {
            report_service_status(SERVICE_STOPPED);
            return;
        }
        if (built.error != ERROR_SUCCESS || built.failures != 0 ||
            built.records.empty()) {
            const DWORD error = built.error == ERROR_SUCCESS
                ? ERROR_READ_FAULT : built.error;
            log_event(EVENTLOG_ERROR_TYPE,
                      L"Initial multi-volume MFT enumeration failed, error=" +
                          std::to_wstring(error));
            report_service_status(SERVICE_STOPPED, error, 0, error);
            return;
        }
        const auto count = built.records.size();
        current_generation = create_snapshot_generation();
        // Persist while the reconciliation records already exist, then let the
        // index consume and release them before allocating its accelerators.
        // This avoids retaining a second multi-million-record path vector.
        const auto persisted = persist_mft_auto_generation(
            snapshot_path, built.records, built.states, current_generation);
        if (!persisted.snapshot.ok) {
            log_event(EVENTLOG_WARNING_TYPE,
                      L"Initial multi-volume MFT snapshot save failed, error=" +
                          std::to_wstring(persisted.snapshot.error));
        } else {
            if (!persisted.hydration_wal.ok) {
                log_event(
                    EVENTLOG_WARNING_TYPE,
                    L"Initial metadata hydration WAL creation failed, error=" +
                        std::to_wstring(persisted.hydration_wal.error));
            }
            if (!persisted.state.ok) {
                log_event(
                    EVENTLOG_WARNING_TYPE,
                    L"Initial multi-volume startup state save failed, error=" +
                        std::to_wstring(persisted.state.error));
            }
        }
        index.replace(std::move(built.records));
        initial_states = std::move(built.states);
        startup_rebuild_required = false;
        initial_hydration_scheduled = true;
        initial_hydration_wal_ready = persisted.hydration_wal.ok;
        log_event(EVENTLOG_INFORMATION_TYPE,
                  L"Indexed " + std::to_wstring(count) + L" entries on " +
                      join_volumes(built.volumes) +
                      L"; name index published; background metadata "
                      L"hydration scheduled");
    }

    release_transient_process_memory();
    report_service_status(SERVICE_RUNNING);
    std::jthread coordinator(
        [&, states = std::move(initial_states), startup_rebuild_required,
         initial_hydration_scheduled, initial_hydration_wal_ready,
         initial_hydration_after_id, current_generation]
        (std::stop_token token) mutable {
        bool rebuild_required = startup_rebuild_required || states.empty();
        bool metadata_reuse_allowed =
            !startup_rebuild_required && !states.empty();
        BackgroundMetadataHydration metadata_hydration;
        if (initial_hydration_scheduled) {
            start_background_metadata_hydration(
                metadata_hydration, current_generation, hydration_wal_path,
                initial_hydration_wal_ready, initial_hydration_after_id);
        }
        auto next_reconciliation_attempt = std::chrono::steady_clock::now();
        auto next_volume_discovery = std::chrono::steady_clock::now() +
                                     mft_reconciliation_interval;
        auto last_delta_checkpoint = std::chrono::steady_clock::now();
        std::size_t changes_since_checkpoint = 0;
        while (!token.stop_requested() &&
               !runtime.stop.load(std::memory_order_relaxed)) {
            const auto now = std::chrono::steady_clock::now();
            if (rebuild_required && now >= next_reconciliation_attempt) {
                auto built = build_all_ntfs_volumes();
                if (built.error == ERROR_SUCCESS && built.failures == 0 &&
                    !built.records.empty()) {
                    const auto count = built.records.size();
                    const auto volumes = join_volumes(built.volumes);
                    bool reuse_attempted = false;
                    if (metadata_reuse_allowed) {
                        std::wstring failed_volume;
                        const auto catch_up_error =
                            catch_up_metadata_reuse_sources(
                                states, built.states, index, snapshot_path,
                                current_generation, failed_volume);
                        if (catch_up_error == ERROR_SUCCESS) {
                            reuse_attempted = true;
                        } else {
                            metadata_reuse_allowed = false;
                            log_event(
                                EVENTLOG_WARNING_TYPE,
                                L"Pre-reconciliation USN catch-up failed for " +
                                    failed_volume + L", error=" +
                                    std::to_wstring(catch_up_error) +
                                    L"; disabling metadata reuse for this "
                                    L"generation");
                        }
                    }
                    esm::MetadataReuseStats reused_metadata;
                    reused_metadata.examined = count;
                    if (reuse_attempted) {
                        reused_metadata =
                            index.reuse_search_metadata(built.records);
                    } else {
                        reused_metadata.unknown = count;
                    }
                    // Reuse the records already produced by reconciliation for
                    // persistence. Generating another full-path snapshot from
                    // the live catalogs caused the service to jump back into
                    // the multi-gigabyte range every five minutes.
                    const auto old_generation = current_generation;
                    const auto old_states = states;
                    const auto next_generation =
                        create_snapshot_generation();
                    const auto persisted = persist_mft_auto_generation(
                        snapshot_path, built.records, built.states,
                        next_generation);
                    if (!persisted.snapshot.ok) {
                        log_event(EVENTLOG_WARNING_TYPE,
                                  L"Multi-volume MFT snapshot save failed, "
                                  L"error=" +
                                      std::to_wstring(
                                          persisted.snapshot.error));
                    } else {
                        if (!persisted.hydration_wal.ok) {
                            log_event(
                                EVENTLOG_WARNING_TYPE,
                                L"Metadata hydration WAL creation failed, "
                                L"error=" +
                                    std::to_wstring(
                                        persisted.hydration_wal.error));
                        }
                        if (!persisted.metadata_wals_ok) {
                            log_event(
                                EVENTLOG_WARNING_TYPE,
                                L"Multi-volume name/USN WAL initialization "
                                L"failed, error=" +
                                    std::to_wstring(
                                        persisted.metadata_wal_error));
                        }
                        if (!persisted.state.ok) {
                            log_event(
                                EVENTLOG_WARNING_TYPE,
                                L"Multi-volume startup state save failed, "
                                L"error=" +
                                    std::to_wstring(persisted.state.error));
                        }
                    }
                    index.replace(std::move(built.records));
                    states = std::move(built.states);
                    current_generation = next_generation;
                    if (persisted.committed()) {
                        remove_generation_metadata_wals(
                            snapshot_path, old_generation, old_states);
                    }
                    changes_since_checkpoint = 0;
                    last_delta_checkpoint = std::chrono::steady_clock::now();
                    rebuild_required = false;
                    metadata_reuse_allowed = std::all_of(
                        states.begin(), states.end(),
                        [](const VolumeLiveState& state) { return state.live; });
                    start_background_metadata_hydration(
                        metadata_hydration, current_generation,
                        hydration_wal_path, persisted.hydration_wal.ok);
                    next_volume_discovery = std::chrono::steady_clock::now() +
                                            mft_reconciliation_interval;
                    log_event(EVENTLOG_INFORMATION_TYPE,
                              L"Reconciled " + volumes + L" with " +
                                  std::to_wstring(count) +
                                  L" entries; metadata reuse=" +
                                  (reuse_attempted ? L"trusted" : L"disabled") +
                                  L", reused=" +
                                  std::to_wstring(reused_metadata.reused) +
                                  L", unknown=" +
                                  std::to_wstring(reused_metadata.unknown) +
                                  L", stale=" +
                                  std::to_wstring(reused_metadata.stale) +
                                  L"; name index published; background metadata "
                                  L"hydration scheduled for unresolved entries");
                    release_transient_process_memory();
                } else {
                    next_reconciliation_attempt =
                        std::chrono::steady_clock::now() +
                        mft_reconciliation_interval;
                    log_event(EVENTLOG_WARNING_TYPE,
                              L"Multi-volume MFT reconciliation was "
                              L"incomplete; keeping the previous index");
                }
            }

            if (!rebuild_required) {
                for (auto& state : states) {
                    if (!state.live) continue;
                    const auto error = catch_up_volume(
                        state, index, snapshot_path, current_generation,
                        &changes_since_checkpoint);
                    if (error != ERROR_SUCCESS) {
                        log_event(EVENTLOG_WARNING_TYPE,
                                  L"USN polling failed for " +
                                      state.volume.root + L", error=" +
                                      std::to_wstring(error) +
                                      L"; scheduling an MFT reconciliation");
                        rebuild_required = true;
                        metadata_reuse_allowed = false;
                        next_reconciliation_attempt =
                            std::chrono::steady_clock::now();
                        break;
                    }
                }

                const auto checkpoint_now =
                    std::chrono::steady_clock::now();
                if (!rebuild_required && changes_since_checkpoint != 0 &&
                    (changes_since_checkpoint >=
                         mft_delta_checkpoint_changes ||
                     checkpoint_now - last_delta_checkpoint >=
                         mft_delta_checkpoint_interval)) {
                    const auto old_generation = current_generation;
                    const auto old_states = states;
                    auto checkpoint_records = index.snapshot_records();
                    const auto next_generation =
                        create_snapshot_generation();
                    const auto persisted = persist_mft_auto_generation(
                        snapshot_path, checkpoint_records, states,
                        next_generation);
                    if (persisted.committed()) {
                        const auto count = checkpoint_records.size();
                        index.replace(std::move(checkpoint_records));
                        current_generation = next_generation;
                        changes_since_checkpoint = 0;
                        last_delta_checkpoint = checkpoint_now;
                        remove_generation_metadata_wals(
                            snapshot_path, old_generation, old_states);
                        start_background_metadata_hydration(
                            metadata_hydration, current_generation,
                            hydration_wal_path, persisted.hydration_wal.ok);
                        log_event(
                            EVENTLOG_INFORMATION_TYPE,
                            L"Consolidated multi-volume checkpoint: entries=" +
                                std::to_wstring(count) + L", old generation=" +
                                std::to_wstring(old_generation) +
                                L", new generation=" +
                                std::to_wstring(current_generation));
                        release_transient_process_memory();
                    } else {
                        last_delta_checkpoint = checkpoint_now;
                        log_event(
                            EVENTLOG_WARNING_TYPE,
                            L"Multi-volume checkpoint consolidation failed; "
                            L"snapshot error=" +
                                std::to_wstring(persisted.snapshot.error) +
                                L", name WAL error=" +
                                std::to_wstring(
                                    persisted.metadata_wal_error) +
                                L", state error=" +
                                std::to_wstring(persisted.state.error) +
                                L"; retaining the active generation and WALs");
                    }
                }

                if (!rebuild_required &&
                    std::chrono::steady_clock::now() >=
                        next_volume_discovery) {
                    const auto discovery =
                        esm::discover_mounted_ntfs_volumes();
                    if (discovery.error == ERROR_SUCCESS &&
                        !same_mounted_volumes(states, discovery.volumes)) {
                        log_event(EVENTLOG_INFORMATION_TYPE,
                                  L"Mounted NTFS volume set changed; "
                                  L"scheduling an MFT reconciliation");
                        rebuild_required = true;
                        next_reconciliation_attempt =
                            std::chrono::steady_clock::now();
                    } else if (discovery.error != ERROR_SUCCESS) {
                        log_event(EVENTLOG_WARNING_TYPE,
                                  L"Mounted NTFS volume discovery failed, "
                                  L"error=" +
                                      std::to_wstring(discovery.error));
                    }
                    next_volume_discovery =
                        std::chrono::steady_clock::now() +
                        mft_reconciliation_interval;
                }
            }

            if (!rebuild_required && metadata_hydration.active &&
                !token.stop_requested() &&
                !runtime.stop.load(std::memory_order_relaxed)) {
                run_background_metadata_hydration_batch(
                    index, metadata_hydration);
            }

            std::this_thread::sleep_for(
                !rebuild_required && metadata_hydration.active
                    ? metadata_hydration_poll_interval
                    : mft_live_poll_interval);
        }
        if (metadata_hydration.active) {
            log_event(EVENTLOG_INFORMATION_TYPE,
                      background_metadata_hydration_summary(
                          L"Background metadata hydration stopped before "
                          L"completion",
                          metadata_hydration));
        }
    });

    const auto pipe_error = esm::serve_named_pipe_search(
        configuration.pipe_name, index, runtime.stop, 4,
        log_pipe_search_diagnostics);
    report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 30'000);
    coordinator.request_stop();
    if (coordinator.joinable()) coordinator.join();

    if (pipe_error != ERROR_SUCCESS) {
        log_event(EVENTLOG_ERROR_TYPE,
                  L"Multi-volume MFT service stopped with pipe error=" +
                      std::to_wstring(pipe_error));
        report_service_status(SERVICE_STOPPED, pipe_error, 0, pipe_error);
        return;
    }
    log_event(EVENTLOG_INFORMATION_TYPE, L"Search service stopped cleanly");
    report_service_status(SERVICE_STOPPED);
}

void run_mft_service() {
    auto startup = std::async(std::launch::async, [] {
        return esm::enumerate_ntfs_volume(configuration.volume.wstring());
    });
    while (startup.wait_for(std::chrono::seconds(1)) !=
           std::future_status::ready) {
        if (runtime.stop.load(std::memory_order_relaxed))
            report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 120'000);
        else
            report_service_status(SERVICE_START_PENDING, ERROR_SUCCESS, 120'000);
    }

    auto scan = startup.get();
    if (scan.records.empty() && scan.errors != 0) {
        constexpr DWORD error = ERROR_READ_FAULT;
        log_event(EVENTLOG_ERROR_TYPE,
                  L"MFT enumeration failed for " +
                      configuration.volume.wstring());
        report_service_status(SERVICE_STOPPED, error, 0, error);
        return;
    }
    if (runtime.stop.load(std::memory_order_relaxed)) {
        report_service_status(SERVICE_STOPPED);
        return;
    }

    esm::MetadataIndex index;
    index.replace(std::move(scan.records));
    log_event(EVENTLOG_INFORMATION_TYPE,
              L"Search service started with " +
                  std::to_wstring(index.size()) +
                  L" entries via MFT enumeration; metadata hydrated=" +
                  std::to_wstring(scan.metadata_hydrated) + L", errors=" +
                  std::to_wstring(scan.metadata_errors) + L", elapsed=" +
                  std::to_wstring(scan.metadata_elapsed.count()) + L" ms");
    report_service_status(SERVICE_RUNNING);

    std::atomic<DWORD> reconciliation_error{ERROR_SUCCESS};
    std::jthread reconciler([&](std::stop_token token) {
        auto next_refresh = std::chrono::steady_clock::now() +
                            mft_reconciliation_interval;
        while (!token.stop_requested() &&
               !runtime.stop.load(std::memory_order_relaxed)) {
            if (std::chrono::steady_clock::now() < next_refresh) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            auto refreshed =
                esm::enumerate_ntfs_volume(configuration.volume.wstring());
            if (refreshed.records.empty() && refreshed.errors != 0) {
                reconciliation_error.store(ERROR_READ_FAULT,
                                           std::memory_order_relaxed);
                log_event(EVENTLOG_WARNING_TYPE,
                          L"Periodic MFT reconciliation failed; keeping the "
                          L"previous searchable index");
            } else {
                index.replace(std::move(refreshed.records));
                reconciliation_error.store(ERROR_SUCCESS,
                                           std::memory_order_relaxed);
            }
            next_refresh = std::chrono::steady_clock::now() +
                           mft_reconciliation_interval;
        }
    });

    const auto pipe_error = esm::serve_named_pipe_search(
        configuration.pipe_name, index, runtime.stop, 4,
        log_pipe_search_diagnostics);
    report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 30'000);
    reconciler.request_stop();
    if (reconciler.joinable()) reconciler.join();

    if (pipe_error != ERROR_SUCCESS) {
        log_event(EVENTLOG_ERROR_TYPE,
                  L"MFT search service stopped with pipe error=" +
                      std::to_wstring(pipe_error));
        report_service_status(SERVICE_STOPPED, pipe_error, 0, pipe_error);
        return;
    }
    (void)reconciliation_error;
    log_event(EVENTLOG_INFORMATION_TYPE, L"Search service stopped cleanly");
    report_service_status(SERVICE_STOPPED);
}

void run_live_service() {
    esm::LiveIndexSession session;
    auto startup = std::async(std::launch::async, [&] {
        return session.start(configuration.volume, configuration.checkpoint);
    });
    while (startup.wait_for(std::chrono::seconds(1)) !=
           std::future_status::ready) {
        if (runtime.stop.load(std::memory_order_relaxed))
            report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 120'000);
        else
            report_service_status(SERVICE_START_PENDING, ERROR_SUCCESS, 120'000);
    }

    const auto started = startup.get();
    if (!started.ok) {
        log_event(EVENTLOG_ERROR_TYPE, service_start_failure(started));
        const DWORD error = started.error == ERROR_SUCCESS
            ? ERROR_SERVICE_SPECIFIC_ERROR : started.error;
        report_service_status(SERVICE_STOPPED, error, 0, started.error);
        return;
    }
    if (runtime.stop.load(std::memory_order_relaxed)) {
        report_service_status(SERVICE_STOPPED);
        return;
    }

    std::atomic<DWORD> follower_error{ERROR_SUCCESS};
    std::jthread follower([&](std::stop_token token) {
        std::size_t changes_since_snapshot = 0;
        auto last_snapshot = std::chrono::steady_clock::now();
        while (!token.stop_requested() &&
               !runtime.stop.load(std::memory_order_relaxed)) {
            const auto poll = session.poll_once();
            if (!poll.ok) {
                follower_error.store(poll.error, std::memory_order_relaxed);
                runtime.stop.store(true, std::memory_order_relaxed);
                log_event(EVENTLOG_ERROR_TYPE,
                          L"USN journal follower failed, error=" +
                              std::to_wstring(poll.error));
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (poll.checkpoint_consolidated) {
                changes_since_snapshot = 0;
                last_snapshot = now;
            } else {
                changes_since_snapshot += poll.changes;
            }
            if (!poll.checkpoint_consolidated &&
                (changes_since_snapshot >= snapshot_refresh_changes ||
                 now - last_snapshot >= snapshot_refresh_interval)) {
                const auto saved = session.save_snapshot();
                if (!saved.ok) {
                    follower_error.store(saved.error,
                                         std::memory_order_relaxed);
                    runtime.stop.store(true, std::memory_order_relaxed);
                    log_event(EVENTLOG_ERROR_TYPE,
                              L"Metadata snapshot refresh failed, error=" +
                                  std::to_wstring(saved.error));
                    break;
                }
                changes_since_snapshot = 0;
                last_snapshot = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    log_event(EVENTLOG_INFORMATION_TYPE,
              L"Search service started with " +
                  std::to_wstring(session.index().size()) + L" entries via " +
                  (started.snapshot_loaded ? L"metadata snapshot"
                                           : L"MFT enumeration"));
    report_service_status(SERVICE_RUNNING);
    const auto pipe_error = esm::serve_named_pipe_search(
        configuration.pipe_name, session.index(), runtime.stop, 4,
        log_pipe_search_diagnostics);

    report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 30'000);
    follower.request_stop();
    if (follower.joinable()) follower.join();

    DWORD error = follower_error.load(std::memory_order_relaxed);
    if (error == ERROR_SUCCESS) error = pipe_error;
    if (error == ERROR_SUCCESS) {
        const auto saved = session.save_snapshot();
        if (!saved.ok) {
            error = saved.error;
            log_event(EVENTLOG_ERROR_TYPE,
                      L"Final metadata snapshot save failed, error=" +
                          std::to_wstring(saved.error));
        }
    }
    if (error != ERROR_SUCCESS) {
        log_event(EVENTLOG_ERROR_TYPE,
                  L"Search service stopped with error=" +
                      std::to_wstring(error));
        report_service_status(SERVICE_STOPPED, error, 0, error);
        return;
    }
    log_event(EVENTLOG_INFORMATION_TYPE, L"Search service stopped cleanly");
    report_service_status(SERVICE_STOPPED);
}

void WINAPI service_main(DWORD, LPWSTR*) {
    runtime.handle = RegisterServiceCtrlHandlerExW(
        service_name, service_control_handler, nullptr);
    if (runtime.handle == nullptr) return;

    runtime.stop.store(false, std::memory_order_relaxed);
    report_service_status(SERVICE_START_PENDING, ERROR_SUCCESS, 120'000);
    if (configuration.mode == ServiceMode::mft)
        run_mft_service();
    else if (configuration.mode == ServiceMode::mft_auto)
        run_mft_auto_service();
    else
        run_live_service();
}

std::wstring quote_argument(std::wstring_view argument) {
    std::wstring result;
    result.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::filesystem::path executable_path() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD size = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) return {};
        if (size < buffer.size() - 1)
            return std::filesystem::path(
                std::wstring(buffer.data(), static_cast<std::size_t>(size)));
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring format_error(DWORD error) {
    wchar_t* text = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = size == 0 ? L"" : std::wstring(text, size);
    if (text != nullptr) LocalFree(text);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n'))
        message.pop_back();
    return message;
}

void print_error(std::wstring_view operation, DWORD error) {
    std::wcerr << operation << L" failed, error=" << error;
    const auto text = format_error(error);
    if (!text.empty()) std::wcerr << L" (" << text << L")";
    std::wcerr << L"\n";
}

bool query_status(SC_HANDLE service, SERVICE_STATUS_PROCESS& status,
                  DWORD& error) {
    DWORD needed = 0;
    if (!QueryServiceStatusEx(
            service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
        error = GetLastError();
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

DWORD wait_for_state(SC_HANDLE service, DWORD desired,
                     DWORD timeout_ms = service_wait_timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    SERVICE_STATUS_PROCESS status{};
    DWORD error = ERROR_SUCCESS;
    for (;;) {
        if (!query_status(service, status, error)) return error;
        if (status.dwCurrentState == desired) return ERROR_SUCCESS;
        if (desired == SERVICE_RUNNING &&
            status.dwCurrentState == SERVICE_STOPPED) {
            return status.dwWin32ExitCode == ERROR_SUCCESS
                ? ERROR_SERVICE_NOT_ACTIVE : status.dwWin32ExitCode;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return ERROR_TIMEOUT;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

DWORD install_service(const ServiceConfiguration& config) {
    std::filesystem::path checkpoint;
    if (config.mode == ServiceMode::live) {
        const auto comparison = esm::compare_path_volumes(
            config.volume, config.checkpoint);
        if (!comparison.ok) return comparison.error;
        if (comparison.same_volume) {
            std::wcerr << L"checkpoint must be stored outside the indexed "
                          L"volume to avoid a self-generated USN loop\n";
            return ERROR_INVALID_PARAMETER;
        }
        checkpoint = std::filesystem::absolute(config.checkpoint)
                         .lexically_normal();
    }

    const auto executable = executable_path();
    if (executable.empty()) return GetLastError();
    std::wstring command = quote_argument(executable.wstring());
    if (config.mode == ServiceMode::mft) {
        command += L" service-mft " +
                   quote_argument(config.volume.wstring()) + L" " +
                   quote_argument(config.pipe_name);
    } else if (config.mode == ServiceMode::mft_auto) {
        std::error_code create_error;
        const auto data_directory = std::filesystem::absolute(
            config.data_directory).lexically_normal();
        std::filesystem::create_directories(data_directory, create_error);
        if (create_error)
            return static_cast<DWORD>(create_error.value());
        command += L" service-mft-auto " +
                   quote_argument(data_directory.wstring()) + L" " +
                   quote_argument(config.pipe_name);
    } else {
        command += L" service " + quote_argument(config.volume.wstring()) +
                   L" " + quote_argument(checkpoint.wstring()) + L" " +
                   quote_argument(config.pipe_name);
    }

    ServiceHandle manager{OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)};
    if (manager.value == nullptr) return GetLastError();
    ServiceHandle service{CreateServiceW(
        manager.value, service_name, service_display_name,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr)};
    if (service.value == nullptr) return GetLastError();

    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<LPWSTR>(
        L"Indexes NTFS file metadata and serves local low-latency searches.");
    ChangeServiceConfig2W(service.value, SERVICE_CONFIG_DESCRIPTION,
                          &description);
    const wchar_t* mode_name = config.mode == ServiceMode::mft_auto
        ? L"mft-auto" : (config.mode == ServiceMode::mft ? L"mft" : L"live");
    std::wcout << L"Installed " << service_display_name << L"\n"
               << L"  mode: " << mode_name << L"\n";
    if (config.mode == ServiceMode::mft_auto)
        std::wcout << L"  data directory: "
                   << std::filesystem::absolute(config.data_directory)
                          .lexically_normal().wstring() << L"\n";
    else
        std::wcout << L"  volume: " << config.volume.wstring() << L"\n";
    if (config.mode == ServiceMode::live)
        std::wcout << L"  checkpoint: " << checkpoint.wstring() << L"\n";
    std::wcout << L"  pipe: " << config.pipe_name << L"\n";
    return ERROR_SUCCESS;
}

DWORD open_service_with_access(DWORD access, ServiceHandle& manager,
                               ServiceHandle& service) {
    manager.value = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager.value == nullptr) return GetLastError();
    service.value = OpenServiceW(manager.value, service_name, access);
    if (service.value == nullptr) return GetLastError();
    return ERROR_SUCCESS;
}

DWORD start_service_command() {
    ServiceHandle manager;
    ServiceHandle service;
    DWORD error = open_service_with_access(
        SERVICE_START | SERVICE_QUERY_STATUS, manager, service);
    if (error != ERROR_SUCCESS) return error;
    if (!StartServiceW(service.value, 0, nullptr)) {
        error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) return error;
    }
    error = wait_for_state(service.value, SERVICE_RUNNING);
    if (error == ERROR_SUCCESS)
        std::wcout << L"Service is running\n";
    return error;
}

DWORD stop_service_command(bool quiet_if_stopped = false) {
    ServiceHandle manager;
    ServiceHandle service;
    DWORD error = open_service_with_access(
        SERVICE_STOP | SERVICE_QUERY_STATUS, manager, service);
    if (error != ERROR_SUCCESS) return error;

    SERVICE_STATUS_PROCESS current{};
    if (!query_status(service.value, current, error)) return error;
    if (current.dwCurrentState == SERVICE_STOPPED) {
        if (!quiet_if_stopped) std::wcout << L"Service is already stopped\n";
        return ERROR_SUCCESS;
    }
    SERVICE_STATUS status{};
    if (!ControlService(service.value, SERVICE_CONTROL_STOP, &status)) {
        error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) return error;
    }
    error = wait_for_state(service.value, SERVICE_STOPPED);
    if (error == ERROR_SUCCESS && !quiet_if_stopped)
        std::wcout << L"Service is stopped\n";
    return error;
}

DWORD uninstall_service() {
    DWORD error = stop_service_command(true);
    if (error != ERROR_SUCCESS && error != ERROR_SERVICE_DOES_NOT_EXIST)
        return error;

    ServiceHandle manager;
    ServiceHandle service;
    error = open_service_with_access(DELETE, manager, service);
    if (error != ERROR_SUCCESS) return error;
    if (!DeleteService(service.value)) return GetLastError();
    std::wcout << L"Uninstalled " << service_display_name << L"\n";
    return ERROR_SUCCESS;
}

const wchar_t* state_name(DWORD state) {
    switch (state) {
    case SERVICE_STOPPED: return L"stopped";
    case SERVICE_START_PENDING: return L"start-pending";
    case SERVICE_STOP_PENDING: return L"stop-pending";
    case SERVICE_RUNNING: return L"running";
    case SERVICE_CONTINUE_PENDING: return L"continue-pending";
    case SERVICE_PAUSE_PENDING: return L"pause-pending";
    case SERVICE_PAUSED: return L"paused";
    default: return L"unknown";
    }
}

DWORD status_service() {
    ServiceHandle manager;
    ServiceHandle service;
    DWORD error = open_service_with_access(
        SERVICE_QUERY_STATUS, manager, service);
    if (error != ERROR_SUCCESS) return error;
    SERVICE_STATUS_PROCESS status{};
    if (!query_status(service.value, status, error)) return error;
    std::wcout << L"Service state: " << state_name(status.dwCurrentState)
               << L"\n"
               << L"Process ID: " << status.dwProcessId << L"\n"
               << L"Win32 exit code: " << status.dwWin32ExitCode << L"\n";
    return ERROR_SUCCESS;
}

void usage() {
    std::wcout
        << L"Usage:\n"
        << L"  esm_service install <volume> <checkpoint-file> [pipe-name]\n"
        << L"  esm_service install-mft <volume> [pipe-name]\n"
        << L"  esm_service install-mft-auto <data-directory> [pipe-name]\n"
        << L"  esm_service uninstall\n"
        << L"  esm_service start\n"
        << L"  esm_service stop\n"
        << L"  esm_service status\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(service_name), service_main},
            {nullptr, nullptr},
        };
        if (!StartServiceCtrlDispatcherW(table)) {
            const DWORD error = GetLastError();
            if (error != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
                print_error(L"StartServiceCtrlDispatcher", error);
            else
                usage();
            return 1;
        }
        return 0;
    }

    const std::wstring_view command(argv[1]);
    if (command == L"service") {
        if (argc < 4 || argc > 5) return 2;
        configuration.mode = ServiceMode::live;
        configuration.volume = argv[2];
        configuration.checkpoint = argv[3];
        configuration.pipe_name = argc == 5 ? argv[4] : default_pipe_name;
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(service_name), service_main},
            {nullptr, nullptr},
        };
        if (!StartServiceCtrlDispatcherW(table)) {
            print_error(L"StartServiceCtrlDispatcher", GetLastError());
            return 1;
        }
        return 0;
    }
    if (command == L"service-mft-auto") {
        if (argc < 3 || argc > 4) return 2;
        configuration.mode = ServiceMode::mft_auto;
        configuration.data_directory = argv[2];
        configuration.pipe_name = argc == 4 ? argv[3] : default_pipe_name;
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(service_name), service_main},
            {nullptr, nullptr},
        };
        if (!StartServiceCtrlDispatcherW(table)) {
            print_error(L"StartServiceCtrlDispatcher", GetLastError());
            return 1;
        }
        return 0;
    }
    if (command == L"service-mft") {
        if (argc < 3 || argc > 4) return 2;
        configuration.mode = ServiceMode::mft;
        configuration.volume = argv[2];
        configuration.pipe_name = argc == 4 ? argv[3] : default_pipe_name;
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(service_name), service_main},
            {nullptr, nullptr},
        };
        if (!StartServiceCtrlDispatcherW(table)) {
            print_error(L"StartServiceCtrlDispatcher", GetLastError());
            return 1;
        }
        return 0;
    }

    DWORD error = ERROR_INVALID_PARAMETER;
    if (command == L"install") {
        if (argc < 4 || argc > 5) {
            usage();
            return 2;
        }
        ServiceConfiguration config;
        config.mode = ServiceMode::live;
        config.volume = argv[2];
        config.checkpoint = argv[3];
        config.pipe_name = argc == 5 ? argv[4] : default_pipe_name;
        error = install_service(config);
    } else if (command == L"install-mft-auto") {
        if (argc < 3 || argc > 4) {
            usage();
            return 2;
        }
        ServiceConfiguration config;
        config.mode = ServiceMode::mft_auto;
        config.data_directory = argv[2];
        config.pipe_name = argc == 4 ? argv[3] : default_pipe_name;
        error = install_service(config);
    } else if (command == L"install-mft") {
        if (argc < 3 || argc > 4) {
            usage();
            return 2;
        }
        ServiceConfiguration config;
        config.mode = ServiceMode::mft;
        config.volume = argv[2];
        config.pipe_name = argc == 4 ? argv[3] : default_pipe_name;
        error = install_service(config);
    } else if (command == L"uninstall") {
        if (argc != 2) return 2;
        error = uninstall_service();
    } else if (command == L"start") {
        if (argc != 2) return 2;
        error = start_service_command();
    } else if (command == L"stop") {
        if (argc != 2) return 2;
        error = stop_service_command();
    } else if (command == L"status") {
        if (argc != 2) return 2;
        error = status_service();
    } else {
        usage();
        return 2;
    }

    if (error != ERROR_SUCCESS) {
        print_error(command, error);
        return 1;
    }
    return 0;
}

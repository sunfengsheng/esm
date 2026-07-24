#include "esm/index.hpp"
#include "esm/live_index.hpp"
#include "esm/journal_checkpoint.hpp"
#include "esm/metadata_snapshot.hpp"
#include "esm/named_pipe.hpp"
#include "esm/ntfs_catalog.hpp"
#include "esm/ntfs_enumerator.hpp"
#include "esm/usn_journal.hpp"
#include "esm/volume_discovery.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
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
constexpr DWORD service_wait_timeout_ms = 120'000;
constexpr auto snapshot_refresh_interval = std::chrono::minutes(5);
constexpr std::size_t snapshot_refresh_changes = 100'000;
constexpr auto mft_reconciliation_interval = std::chrono::minutes(1);
constexpr auto mft_full_reconciliation_interval = std::chrono::minutes(30);
constexpr auto mft_live_poll_interval = std::chrono::milliseconds(250);
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

void log_event(WORD type, std::wstring_view message) {
    HANDLE source = RegisterEventSourceW(nullptr, service_name);
    if (source == nullptr) return;
    std::wstring stable(message);
    LPCWSTR strings[] = {stable.c_str()};
    ReportEventW(source, type, 0, 1, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
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
    std::unique_ptr<esm::NtfsCatalog> catalog;
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

std::wstring join_volumes(const std::vector<std::wstring>& volumes) {
    std::wstring result;
    for (const auto& volume : volumes) {
        if (!result.empty()) result += L", ";
        result += volume;
    }
    return result.empty() ? L"(none)" : result;
}

DWORD catch_up_volume(VolumeLiveState& state,
                      esm::MetadataIndex* index = nullptr) {
    if (!state.live || !state.catalog) return ERROR_NOT_SUPPORTED;
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

        auto delta = state.catalog->apply(batch);
        if (index != nullptr) {
            esm::namespace_ntfs_records(state.volume.identity,
                                        delta.upserts);
            esm::namespace_ntfs_file_ids(state.volume.identity,
                                         delta.removed_ids);
            index->apply_delta(std::move(delta.upserts),
                               delta.removed_ids);
        }
        state.cursor = batch.next_usn;
    }
    return ERROR_SUCCESS;
}

VolumeBuildResult build_volume_live_state(esm::NtfsVolumeInfo volume) {
    VolumeBuildResult result;
    result.state.volume = std::move(volume);

    const auto boundary =
        esm::query_usn_journal(result.state.volume.root);
    auto scan = esm::enumerate_ntfs_volume(result.state.volume.root);
    if (scan.records.empty() && scan.errors != 0) {
        result.error = ERROR_READ_FAULT;
        return result;
    }

    result.state.catalog = std::make_unique<esm::NtfsCatalog>(
        result.state.volume.root, scan.root_id);
    result.state.catalog->replace(scan.records);
    scan.records.clear();
    scan.records.shrink_to_fit();

    if (boundary.available) {
        result.state.journal_id = boundary.journal_id;
        result.state.cursor = boundary.next_usn;
        result.state.live = true;
        const auto catch_up_error = catch_up_volume(result.state);
        if (catch_up_error != ERROR_SUCCESS) {
            result.state.live = false;
            log_event(EVENTLOG_WARNING_TYPE,
                      L"USN catch-up unavailable for " +
                          result.state.volume.root + L", error=" +
                          std::to_wstring(catch_up_error) +
                          L"; periodic MFT reconciliation will be used");
        }
    }

    result.records = result.state.catalog->snapshot();
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
            result.volumes.push_back(item.root);
            result.records.insert(
                result.records.end(),
                std::make_move_iterator(built.records.begin()),
                std::make_move_iterator(built.records.end()));
            result.states.push_back(std::move(built.state));
        } catch (const std::exception&) {
            ++result.failures;
            log_event(EVENTLOG_WARNING_TYPE,
                      L"MFT enumeration raised an exception for " +
                          item.root);
        }
    }
    if (result.volumes.empty()) result.error = ERROR_READ_FAULT;
    return result;
}

bool valid_mft_auto_snapshot(const esm::MetadataSnapshot& snapshot) {
    return snapshot.volume == mft_auto_snapshot_marker &&
           !snapshot.records.empty();
}

esm::MetadataSnapshotIoResult save_mft_auto_snapshot(
    const std::filesystem::path& path,
    std::vector<esm::FileRecord>& records) {
    esm::MetadataSnapshot snapshot;
    snapshot.root_id = 1;
    snapshot.volume = mft_auto_snapshot_marker;
    snapshot.records.swap(records);
    const auto result = esm::save_metadata_snapshot_atomic(path, snapshot);
    snapshot.records.swap(records);
    return result;
}

std::vector<esm::FileRecord> snapshot_live_volumes(
    const std::vector<VolumeLiveState>& states) {
    std::vector<esm::FileRecord> records;
    for (const auto& state : states) {
        if (!state.catalog) continue;
        auto volume_records = state.catalog->snapshot();
        esm::namespace_ntfs_records(state.volume.identity, volume_records);
        records.insert(records.end(),
                       std::make_move_iterator(volume_records.begin()),
                       std::make_move_iterator(volume_records.end()));
    }
    return records;
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
    esm::MetadataIndex index;
    bool loaded_snapshot = false;
    auto loaded = esm::load_metadata_snapshot(snapshot_path);
    if (loaded.ok && valid_mft_auto_snapshot(loaded.snapshot)) {
        const auto count = loaded.snapshot.records.size();
        index.replace(std::move(loaded.snapshot.records));
        loaded_snapshot = true;
        log_event(EVENTLOG_INFORMATION_TYPE,
                  L"Loaded " + std::to_wstring(count) +
                      L" entries from the multi-volume MFT snapshot");
    } else if (loaded.ok) {
        log_event(EVENTLOG_WARNING_TYPE,
                  L"Ignoring an incompatible multi-volume MFT snapshot");
    } else if (std::filesystem::exists(snapshot_path)) {
        log_event(EVENTLOG_WARNING_TYPE,
                  L"Could not load the multi-volume MFT snapshot, error=" +
                      std::to_wstring(loaded.error));
    }

    std::vector<VolumeLiveState> initial_states;
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
        index.replace(built.records);
        const auto saved =
            save_mft_auto_snapshot(snapshot_path, built.records);
        if (!saved.ok) {
            log_event(EVENTLOG_WARNING_TYPE,
                      L"Initial multi-volume MFT snapshot save failed, error=" +
                          std::to_wstring(saved.error));
        }
        initial_states = std::move(built.states);
        log_event(EVENTLOG_INFORMATION_TYPE,
                  L"Indexed " + std::to_wstring(count) + L" entries on " +
                      join_volumes(built.volumes));
    }

    report_service_status(SERVICE_RUNNING);
    std::jthread coordinator(
        [&, states = std::move(initial_states), loaded_snapshot]
        (std::stop_token token) mutable {
        bool rebuild_required = loaded_snapshot || states.empty();
        auto next_full_reconciliation = std::chrono::steady_clock::now();
        auto last_snapshot_request = std::chrono::steady_clock::now();
        std::future<esm::MetadataSnapshotIoResult> snapshot_save;

        while (!token.stop_requested() &&
               !runtime.stop.load(std::memory_order_relaxed)) {
            if (snapshot_save.valid() &&
                snapshot_save.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready) {
                const auto saved = snapshot_save.get();
                if (!saved.ok) {
                    log_event(EVENTLOG_WARNING_TYPE,
                              L"Multi-volume MFT snapshot save failed, "
                              L"error=" + std::to_wstring(saved.error));
                } else {
                    log_event(EVENTLOG_INFORMATION_TYPE,
                              L"Saved multi-volume MFT snapshot");
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (rebuild_required || now >= next_full_reconciliation) {
                auto built = build_all_ntfs_volumes();
                if (built.error == ERROR_SUCCESS && built.failures == 0 &&
                    !built.records.empty()) {
                    const auto count = built.records.size();
                    const auto volumes = join_volumes(built.volumes);
                    index.replace(std::move(built.records));
                    states = std::move(built.states);
                    rebuild_required = false;
                    next_full_reconciliation =
                        std::chrono::steady_clock::now() +
                        mft_full_reconciliation_interval;
                    log_event(EVENTLOG_INFORMATION_TYPE,
                              L"Reconciled " + volumes + L" with " +
                                  std::to_wstring(count) + L" entries");
                } else {
                    next_full_reconciliation =
                        std::chrono::steady_clock::now() +
                        mft_reconciliation_interval;
                    log_event(EVENTLOG_WARNING_TYPE,
                              L"Multi-volume MFT reconciliation was "
                              L"incomplete; keeping the previous index");
                }
            }

            if (!rebuild_required) {
                for (auto& state : states) {
                    if (!state.live) {
                        rebuild_required = true;
                        break;
                    }
                    const auto error = catch_up_volume(state, &index);
                    if (error != ERROR_SUCCESS) {
                        log_event(EVENTLOG_WARNING_TYPE,
                                  L"USN polling failed for " +
                                      state.volume.root + L", error=" +
                                      std::to_wstring(error) +
                                      L"; scheduling an MFT reconciliation");
                        rebuild_required = true;
                        break;
                    }
                }
            }

            if (!rebuild_required && !snapshot_save.valid() &&
                std::chrono::steady_clock::now() - last_snapshot_request >=
                    snapshot_refresh_interval) {
                auto records = snapshot_live_volumes(states);
                last_snapshot_request = std::chrono::steady_clock::now();
                snapshot_save = std::async(
                    std::launch::async,
                    [snapshot_path, records = std::move(records)]() mutable {
                        return save_mft_auto_snapshot(snapshot_path, records);
                    });
            }

            std::this_thread::sleep_for(mft_live_poll_interval);
        }

        if (snapshot_save.valid()) {
            const auto saved = snapshot_save.get();
            if (!saved.ok) {
                log_event(EVENTLOG_WARNING_TYPE,
                          L"Final multi-volume snapshot completion failed, "
                          L"error=" + std::to_wstring(saved.error));
            }
        }
    });

    const auto pipe_error = esm::serve_named_pipe_search(
        configuration.pipe_name, index, runtime.stop, 4);
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
                  L" entries via MFT enumeration");
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
        configuration.pipe_name, index, runtime.stop, 4);
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
        configuration.pipe_name, session.index(), runtime.stop, 4);

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

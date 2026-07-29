#include "esm/directory_scanner.hpp"
#include "esm/directory_watcher.hpp"
#include "esm/index.hpp"
#include "esm/live_index.hpp"
#include "esm/named_pipe.hpp"
#include "esm/ntfs_enumerator.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {
std::atomic<std::atomic_bool*> console_stop_flag{nullptr};

BOOL WINAPI console_control_handler(DWORD control) {
    switch (control) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (auto* stop = console_stop_flag.load(std::memory_order_relaxed)) {
            stop->store(true, std::memory_order_relaxed);
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

std::wstring from_utf8(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            size) != size) {
        return {};
    }
    return result;
}

std::string to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            size, nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

void usage() {
    std::cout << "Usage:\n"
              << "  esm_server scan <root> [pipe-name] [--once]\n"
              << "  esm_server mft <volume> [pipe-name] [--once]\n"
              << "  esm_server live <volume> <checkpoint-file> [pipe-name] [--once]\n";
}

void print_poll(const esm::LivePollResult& poll,
                const esm::MetadataIndex& index) {
    if (poll.batches == 0 && !poll.checkpoint_consolidated) return;
    std::cout << "[journal] next=" << poll.next_usn
              << " batches=" << poll.batches
              << " changes=" << poll.changes
              << " create=" << poll.created
              << " update=" << poll.updated
              << " rename=" << poll.renamed
              << " delete=" << poll.deleted
              << " pending_index=" << index.pending_delta_size()
              << " wal_bytes=" << poll.wal_bytes;
    if (poll.checkpoint_consolidated) {
        std::cout << " checkpoint=consolidated"
                  << " checkpoint_ms=" << poll.checkpoint_elapsed.count();
    }
    std::cout << "\n";
}

void print_live_start_error(const esm::LiveStartResult& result) {
    if (result.checkpoint_same_volume) {
        std::cout << "checkpoint must be stored outside the indexed "
                     "volume to avoid a self-generated USN loop\n";
        return;
    }
    if (result.stage == esm::LiveStartStage::catch_up &&
        result.catch_up.rebuild_required) {
        std::cout << "live server requires rebuild: "
                  << esm::checkpoint_status_name(
                         result.catch_up.checkpoint_status)
                  << "\n";
        return;
    }
    std::cout << "live index start failed at "
              << esm::live_start_stage_name(result.stage)
              << ", error=" << result.error << "\n";
}
} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    int argument_count = argc;
    const bool serve_once = argc > 1 &&
        std::string_view(argv[argc - 1]) == "--once";
    if (serve_once) --argument_count;
    if (argument_count < 3) {
        usage();
        return 2;
    }
    const std::string_view mode(argv[1]);
    if (mode != "scan" && mode != "mft" && mode != "live") {
        usage();
        return 2;
    }
    if (mode == "live" && argument_count < 4) {
        usage();
        return 2;
    }

    const std::filesystem::path root = from_utf8(argv[2]);
    if (root.empty()) {
        std::cout << "invalid UTF-8 root/volume\n";
        return 2;
    }
    const std::wstring pipe_name = mode == "live"
        ? (argument_count >= 5 ? from_utf8(argv[4]) : L"everything_sm")
        : (argument_count >= 4 ? from_utf8(argv[3]) : L"everything_sm");
    if (pipe_name.empty()) {
        std::cout << "invalid UTF-8 pipe name\n";
        return 2;
    }

    esm::MetadataIndex static_index;
    esm::LiveIndexSession live_session;
    esm::MetadataIndex* index = &static_index;
    std::atomic_bool stop{false};
    std::atomic<std::uint32_t> follower_error{ERROR_SUCCESS};
    std::jthread follower;
    std::unique_ptr<esm::DirectoryWatcher> directory_watcher;
    std::jthread directory_watch_thread;
    std::jthread directory_refresh_thread;
    std::mutex refresh_mutex;
    std::condition_variable refresh_cv;
    std::uint64_t change_generation = 0;
    auto last_directory_change = std::chrono::steady_clock::now();

    if (mode == "live") {
        const std::filesystem::path checkpoint_path = from_utf8(argv[3]);
        if (checkpoint_path.empty()) {
            std::cout << "invalid UTF-8 checkpoint path\n";
            return 2;
        }

        const auto started = live_session.start(root, checkpoint_path);
        if (!started.ok) {
            print_live_start_error(started);
            return started.stage == esm::LiveStartStage::volume_check ? 2 : 1;
        }
        std::cout << "Bootstrap journal_id=" << started.journal_id
                  << " start_usn=" << started.bootstrap_usn << "\n";
        if (started.snapshot_loaded) {
            std::cout << "Loaded metadata snapshot: " << started.entries
                      << " entries in "
                      << started.snapshot_load_elapsed.count() << " ms\n";
        } else {
            if (started.snapshot_rejected) {
                std::cout << "Metadata snapshot rejected, error="
                          << started.snapshot_error << "; rebuilding MFT\n";
            }
            std::cout << "MFT enumeration/path stage completed for "
                      << started.entries << " entries in "
                      << started.enumeration_elapsed.count() << " ms ("
                      << started.scan_errors << " errors); metadata stage "
                      << "hydrated " << started.metadata_hydrated
                      << " entries in " << started.metadata_elapsed.count()
                      << " ms (" << started.metadata_errors << " errors)\n";
        }
        std::cout << "Search index ready: " << live_session.index().size()
                  << " entries in " << started.index_elapsed.count()
                  << " ms\n";
        if (!started.snapshot_loaded && started.snapshot_saved) {
            std::cout << "Metadata snapshot saved in "
                      << started.snapshot_save_elapsed.count() << " ms\n";
        }
        print_poll(started.catch_up, live_session.index());
        index = &live_session.index();

        follower = std::jthread([&](std::stop_token token) {
            while (!token.stop_requested() &&
                   !stop.load(std::memory_order_relaxed)) {
                const auto poll = live_session.poll_once();
                if (!poll.ok) {
                    follower_error.store(poll.error,
                                         std::memory_order_relaxed);
                    if (poll.rebuild_required) {
                        std::cout << "live server requires rebuild: "
                                  << esm::checkpoint_status_name(
                                         poll.checkpoint_status)
                                  << "\n";
                    }
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                print_poll(poll, live_session.index());
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
    } else {
        if (mode == "scan" && !serve_once) {
            directory_watcher =
                std::make_unique<esm::DirectoryWatcher>(root);
            if (!directory_watcher->ready()) {
                std::cout << "directory watcher start failed, error="
                          << directory_watcher->error() << "\n";
                return 1;
            }
            directory_watch_thread = std::jthread(
                [&](std::stop_token token) {
                    while (!token.stop_requested() &&
                           !stop.load(std::memory_order_relaxed)) {
                        auto batch = directory_watcher->wait(token);
                        if (batch.stopped) break;
                        if (!batch.ok) {
                            follower_error.store(
                                batch.error == ERROR_SUCCESS
                                    ? ERROR_GEN_FAILURE : batch.error,
                                std::memory_order_relaxed);
                            stop.store(true, std::memory_order_relaxed);
                            refresh_cv.notify_all();
                            break;
                        }
                        if (batch.overflowed || !batch.changes.empty()) {
                            {
                                std::lock_guard lock(refresh_mutex);
                                ++change_generation;
                                last_directory_change =
                                    std::chrono::steady_clock::now();
                            }
                            refresh_cv.notify_one();
                        }
                    }
                });
            // Give the watcher thread a chance to issue its first overlapped
            // read before the authoritative initial reconciliation begins.
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        std::cout << (mode == "mft" ? "Enumerating MFT " : "Scanning ")
                  << to_utf8(root.wstring()) << " ...\n";
        auto scan = mode == "mft"
            ? esm::enumerate_ntfs_volume(root.wstring())
            : esm::scan_directories({root});
        std::cout << (mode == "mft" ? "MFT bootstrap completed for "
                                     : "Discovered ")
                  << scan.records.size() << " entries in "
                  << scan.elapsed.count() << " ms (" << scan.errors
                  << " errors)\n";
        if (mode == "mft") {
            std::cout << "Hydrated search metadata for "
                      << scan.metadata_hydrated << " entries in "
                      << scan.metadata_elapsed.count() << " ms ("
                      << scan.metadata_errors << " errors)\n";
        }
        if (scan.records.empty() && scan.errors != 0) {
            directory_watch_thread.request_stop();
            if (directory_watch_thread.joinable())
                directory_watch_thread.join();
            return 1;
        }
        static_index.replace(std::move(scan.records));

        if (mode == "scan" && !serve_once) {
            directory_refresh_thread = std::jthread(
                [&](std::stop_token token) {
                    std::uint64_t applied_generation = 0;
                    std::unique_lock lock(refresh_mutex);
                    while (!token.stop_requested() &&
                           !stop.load(std::memory_order_relaxed)) {
                        refresh_cv.wait(lock, [&] {
                            return token.stop_requested() ||
                                   stop.load(std::memory_order_relaxed) ||
                                   change_generation > applied_generation;
                        });
                        if (token.stop_requested() ||
                            stop.load(std::memory_order_relaxed)) {
                            break;
                        }

                        auto target_generation = change_generation;
                        auto deadline = last_directory_change +
                            std::chrono::milliseconds(150);
                        while (!token.stop_requested() &&
                               !stop.load(std::memory_order_relaxed)) {
                            const bool awakened = refresh_cv.wait_until(
                                lock, deadline, [&] {
                                    return token.stop_requested() ||
                                           stop.load(
                                               std::memory_order_relaxed) ||
                                           change_generation !=
                                               target_generation;
                                });
                            if (!awakened) break;
                            if (token.stop_requested() ||
                                stop.load(std::memory_order_relaxed)) {
                                break;
                            }
                            target_generation = change_generation;
                            deadline = last_directory_change +
                                std::chrono::milliseconds(150);
                        }
                        if (token.stop_requested() ||
                            stop.load(std::memory_order_relaxed)) {
                            break;
                        }

                        lock.unlock();
                        auto refreshed = esm::scan_directories({root});
                        if (refreshed.records.empty() &&
                            refreshed.errors != 0) {
                            follower_error.store(ERROR_READ_FAULT,
                                                 std::memory_order_relaxed);
                            stop.store(true, std::memory_order_relaxed);
                            refresh_cv.notify_all();
                            lock.lock();
                            break;
                        }
                        const auto entries = refreshed.records.size();
                        const auto elapsed = refreshed.elapsed;
                        const auto errors = refreshed.errors;
                        static_index.replace(std::move(refreshed.records));
                        std::cout << "[scan] reconciled " << entries
                                  << " entries in " << elapsed.count()
                                  << " ms (" << errors << " errors)\n";
                        lock.lock();
                        applied_generation = target_generation;
                    }
                });
        }
    }

    std::cout << "Serving " << index->size() << " entries on "
              << to_utf8(esm::normalize_pipe_name(pipe_name));
    if (!serve_once) std::cout << " with 4 pipe workers";
    std::cout << "\n" << std::flush;

    if (serve_once) {
        const auto error = esm::serve_named_pipe_search_once(pipe_name, *index);
        if (error != ERROR_SUCCESS) {
            std::cout << "pipe request failed, error=" << error << "\n";
            return 1;
        }
        const auto live_error = follower_error.load(std::memory_order_relaxed);
        return live_error == ERROR_SUCCESS ? 0 : 1;
    }

    console_stop_flag.store(&stop, std::memory_order_relaxed);
    if (!SetConsoleCtrlHandler(console_control_handler, TRUE)) {
        console_stop_flag.store(nullptr, std::memory_order_relaxed);
        std::cout << "console control handler failed, error="
                  << GetLastError() << "\n";
        return 1;
    }
    const auto pipe_error = esm::serve_named_pipe_search(
        pipe_name, *index, stop, 4);
    SetConsoleCtrlHandler(console_control_handler, FALSE);
    console_stop_flag.store(nullptr, std::memory_order_relaxed);
    follower.request_stop();
    directory_watch_thread.request_stop();
    directory_refresh_thread.request_stop();
    refresh_cv.notify_all();
    if (follower.joinable()) follower.join();
    if (directory_watch_thread.joinable()) directory_watch_thread.join();
    if (directory_refresh_thread.joinable()) directory_refresh_thread.join();

    const auto live_error = follower_error.load(std::memory_order_relaxed);
    if (mode == "live" && live_error == ERROR_SUCCESS &&
        pipe_error == ERROR_SUCCESS) {
        const auto saved = live_session.save_snapshot();
        if (!saved.ok) {
            std::cout << "final metadata snapshot save failed, error="
                      << saved.error << "\n";
            return 1;
        }
        std::cout << "Metadata snapshot saved: " << saved.entries
                  << " entries in " << saved.elapsed.count() << " ms\n";
    }
    if (live_error != ERROR_SUCCESS) {
        std::cout << "background index follower stopped, error="
                  << live_error << "\n";
        return 1;
    }
    if (pipe_error != ERROR_SUCCESS) {
        std::cout << "pipe server stopped, error=" << pipe_error << "\n";
        return 1;
    }
    std::cout << "Server stopped cleanly\n";
    return 0;
}

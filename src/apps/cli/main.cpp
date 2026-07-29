#include "esm/directory_scanner.hpp"
#include "esm/index.hpp"
#include "esm/journal_checkpoint.hpp"
#include "esm/journal_replay.hpp"
#include "esm/ntfs_catalog.hpp"
#include "esm/named_pipe.hpp"
#include "esm/ntfs_enumerator.hpp"
#include "esm/usn_journal.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {
std::wstring from_utf8(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

void execute_query(const esm::MetadataIndex& index,
                   const std::wstring& query) {
    const auto started = std::chrono::steady_clock::now();
    esm::SearchOptions options;
    options.limit = 100;
    options.match_path = false;
    const auto results = index.search(query, options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    std::cout << results.size() << " result(s) in "
              << static_cast<double>(elapsed.count()) / 1000.0 << " ms\n";
    for (const auto& result : results) {
        std::cout << result.score << "\t" << to_utf8(result.record.path)
                  << "\n";
    }
}

void usage() {
    std::cout
        << "Usage:\n"
        << "  esm_cli scan <root> [query]\n"
        << "  esm_cli mft <volume> [query]\n"
        << "  esm_cli journal <volume> [checkpoint-file]\n"
        << "  esm_cli live <volume> <checkpoint-file> [query]\n"
        << "  esm_cli query <pipe-name> <query>\n";
}

const char* checkpoint_status_name(esm::CheckpointStatus status) {
    switch (status) {
    case esm::CheckpointStatus::valid: return "valid";
    case esm::CheckpointStatus::journal_changed: return "journal-changed";
    case esm::CheckpointStatus::expired: return "expired";
    case esm::CheckpointStatus::ahead_of_journal: return "ahead-of-journal";
    }
    return "unknown";
}

bool catch_up_journal(const std::filesystem::path& volume,
                      const std::filesystem::path& checkpoint_path,
                      std::uint64_t journal_id,
                      std::int64_t& cursor,
                      esm::NtfsCatalog& catalog,
                      esm::MetadataIndex& index,
                      std::uint32_t& error) {
    const auto state = esm::query_usn_journal(volume.wstring());
    if (!state.available) {
        error = state.error;
        std::cout << "USN journal unavailable during catch-up, error="
                  << error << "\n";
        return false;
    }
    const auto checkpoint_status = esm::validate_checkpoint(
        state, {journal_id, cursor});
    if (checkpoint_status != esm::CheckpointStatus::valid) {
        error = ERROR_INVALID_DATA;
        std::cout << "live index requires rebuild: "
                  << checkpoint_status_name(checkpoint_status) << "\n";
        return false;
    }

    const auto target = state.next_usn;
    while (cursor < target) {
        const auto batch = esm::read_usn_changes(
            volume.wstring(), state, cursor);
        if (batch.error != ERROR_SUCCESS) {
            error = batch.error;
            std::cout << "journal read failed at " << cursor
                      << ", error=" << error << "\n";
            return false;
        }
        if (batch.next_usn <= cursor) {
            error = ERROR_INVALID_DATA;
            std::cout << "journal reader made no progress at " << cursor
                      << "\n";
            return false;
        }

        const auto replay = esm::replay_journal_batch(
            catalog, index, state, batch, checkpoint_path);
        if (replay.error != ERROR_SUCCESS) {
            error = replay.error;
            std::cout << "journal replay failed at " << cursor
                      << ", error=" << error << "\n";
            return false;
        }
        cursor = replay.next_usn;
        std::cout << "[journal] next=" << cursor
                  << " changes=" << batch.changes.size()
                  << " create=" << replay.created
                  << " update=" << replay.updated
                  << " rename=" << replay.renamed
                  << " delete=" << replay.deleted
                  << " pending_index=" << index.pending_delta_size()
                  << "\n";
    }
    error = ERROR_SUCCESS;
    return true;
}

int run_live(const std::filesystem::path& volume,
             const std::filesystem::path& checkpoint_path,
             const char* one_shot_query) {
    const auto volume_comparison = esm::compare_path_volumes(
        volume, checkpoint_path);
    if (!volume_comparison.ok) {
        std::cout << "checkpoint volume check failed, error="
                  << volume_comparison.error << "\n";
        return 2;
    }
    if (volume_comparison.same_volume) {
        std::cout << "checkpoint must be stored outside the indexed volume "
                     "to avoid a self-generated USN loop\n";
        return 2;
    }

    // Race-free bootstrap: capture a Windows-issued USN boundary before MFT
    // enumeration, then replay everything after that boundary.
    const auto bootstrap_state = esm::query_usn_journal(volume.wstring());
    if (!bootstrap_state.available) {
        std::cout << "USN journal unavailable, error="
                  << bootstrap_state.error << "\n";
        return 1;
    }
    std::int64_t cursor = bootstrap_state.next_usn;
    std::cout << "Bootstrap journal_id=" << bootstrap_state.journal_id
              << " start_usn=" << cursor << "\n";

    auto scan = esm::enumerate_ntfs_volume(volume.wstring());
    std::cout << "MFT bootstrap completed for " << scan.records.size()
              << " entries in " << scan.elapsed.count() << " ms ("
              << scan.errors << " enumeration errors); metadata stage "
              << "hydrated " << scan.metadata_hydrated << " entries in "
              << scan.metadata_elapsed.count() << " ms ("
              << scan.metadata_errors << " errors)\n";
    if (scan.root_id == 0 || (scan.records.empty() && scan.errors != 0)) {
        std::cout << "MFT bootstrap failed\n";
        return 1;
    }

    esm::NtfsCatalog catalog(volume.wstring(), scan.root_id);
    catalog.replace(std::move(scan.records));
    esm::MetadataIndex index;
    const auto index_start = std::chrono::steady_clock::now();
    index.replace(catalog.snapshot());
    const auto index_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - index_start);
    std::cout << "Search index ready: " << index.size() << " entries in "
              << index_ms.count() << " ms\n";

    // The full MFT-derived catalog and index now correspond to a state that
    // can be advanced from the exact pre-enumeration cursor.
    const auto initial_save = esm::save_checkpoint_atomic(
        checkpoint_path, {bootstrap_state.journal_id, cursor});
    if (!initial_save.ok) {
        std::cout << "initial checkpoint save failed, error="
                  << initial_save.error << "\n";
        return 1;
    }

    std::uint32_t catch_up_error = ERROR_SUCCESS;
    if (!catch_up_journal(volume, checkpoint_path,
                          bootstrap_state.journal_id, cursor,
                          catalog, index, catch_up_error)) {
        return 1;
    }

    if (one_shot_query != nullptr) {
        execute_query(index, from_utf8(one_shot_query));
        return 0;
    }

    std::atomic_bool stop{false};
    std::atomic<std::uint32_t> follower_error{ERROR_SUCCESS};
    std::thread follower([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::uint32_t error = ERROR_SUCCESS;
            if (!catch_up_journal(volume, checkpoint_path,
                                  bootstrap_state.journal_id, cursor,
                                  catalog, index, error)) {
                follower_error.store(error, std::memory_order_relaxed);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    std::cout << "Live index active. Enter a query, or an empty line to exit.\n";
    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line) || line.empty()) break;
        execute_query(index, from_utf8(line));
        if (follower_error.load(std::memory_order_relaxed) != ERROR_SUCCESS) {
            std::cout << "journal follower stopped, error="
                      << follower_error.load(std::memory_order_relaxed)
                      << "\n";
            break;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    follower.join();
    return follower_error.load(std::memory_order_relaxed) == ERROR_SUCCESS
        ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::string_view mode(argv[1]);
    if (mode != "scan" && mode != "mft" && mode != "journal" &&
        mode != "live" && mode != "query") {
        usage();
        return 2;
    }
    if (mode == "query") {
        if (argc < 4) {
            usage();
            return 2;
        }
        esm::IpcSearchRequest request;
        request.limit = 100;
        request.match_path = false;
        request.query = from_utf8(argv[3]);
        const auto result = esm::query_named_pipe_search(
            from_utf8(argv[2]), request);
        if (result.error != ERROR_SUCCESS) {
            std::cout << "pipe query failed, error=" << result.error;
            if (!result.protocol_error.empty())
                std::cout << " protocol=" << result.protocol_error;
            std::cout << "\n";
            return 1;
        }
        std::cout << result.response.results.size() << " result(s) in "
                  << static_cast<double>(
                         result.response.elapsed_microseconds) / 1000.0
                  << " ms (server)\n";
        for (const auto& item : result.response.results) {
            std::cout << item.score << "\t"
                      << to_utf8(item.record.path) << "\n";
        }
        return 0;
    }

    const std::filesystem::path root = from_utf8(argv[2]);

    if (mode == "live") {
        if (argc < 4) {
            usage();
            return 2;
        }
        const auto checkpoint_path =
            std::filesystem::path(from_utf8(argv[3]));
        return run_live(root, checkpoint_path, argc >= 5 ? argv[4] : nullptr);
    }

    if (mode == "journal") {
        const auto state = esm::query_usn_journal(root.wstring());
        if (!state.available) {
            std::cout << "USN journal unavailable, error=" << state.error
                      << "\n";
            return 1;
        }
        // A journal read must start at a checkpoint returned by Windows.
        // Arithmetic offsets inside [FirstUsn, NextUsn] are not necessarily
        // record boundaries and can fail with ERROR_INVALID_PARAMETER.
        std::int64_t start = state.first_usn;
        std::filesystem::path checkpoint_path;
        if (argc >= 4) {
            checkpoint_path = from_utf8(argv[3]);
            const auto loaded = esm::load_checkpoint(checkpoint_path);
            if (loaded.ok) {
                const auto status = esm::validate_checkpoint(
                    state, loaded.checkpoint);
                if (status != esm::CheckpointStatus::valid) {
                    std::cout << "checkpoint requires rebuild: "
                              << checkpoint_status_name(status) << "\n";
                    return 3;
                }
                start = loaded.checkpoint.next_usn;
            } else if (loaded.error != ERROR_FILE_NOT_FOUND &&
                       loaded.error != ERROR_PATH_NOT_FOUND) {
                std::cout << "checkpoint load failed, error="
                          << loaded.error << "\n";
                return 3;
            }
        }
        const auto batch = esm::read_usn_changes(root.wstring(), state, start);
        std::cout << "journal_id=" << state.journal_id
                  << " first=" << state.first_usn
                  << " lowest=" << state.lowest_valid_usn
                  << " next=" << state.next_usn
                  << " start=" << start
                  << " request_v="
                  << static_cast<unsigned>(batch.request_version)
                  << " v1_error=" << batch.v1_error
                  << " v0_error=" << batch.v0_error
                  << " changes=" << batch.changes.size()
                  << " error=" << batch.error << "\n";
        for (std::size_t i = 0;
             i < std::min<std::size_t>(50, batch.changes.size()); ++i) {
            const auto& change = batch.changes[i];
            std::cout << change.usn << "\t0x" << std::hex << change.reason
                      << std::dec << "\t" << to_utf8(change.name) << "\n";
        }
        if (batch.error == 0 && !checkpoint_path.empty()) {
            const auto saved = esm::save_checkpoint_atomic(
                checkpoint_path, {state.journal_id, batch.next_usn});
            if (!saved.ok) {
                std::cout << "checkpoint save failed, error="
                          << saved.error << "\n";
                return 1;
            }
            std::cout << "checkpoint_saved=" << batch.next_usn << "\n";
        }
        return batch.error == 0 ? 0 : 1;
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
    if (scan.records.empty() && scan.errors != 0) return 1;
    esm::MetadataIndex index;
    index.replace(std::move(scan.records));
    if (argc >= 4) {
        execute_query(index, from_utf8(argv[3]));
        return 0;
    }
    std::cout << "Enter a query, or an empty line to exit.\n";
    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line) || line.empty()) break;
        execute_query(index, from_utf8(line));
    }
    return 0;
}

#include "esm/metadata_snapshot.hpp"
#include "esm/ntfs_catalog.hpp"

#include <windows.h>
#include <winioctl.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t root_id = 1;

std::size_t working_set_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                              sizeof(counters))) {
        return 0;
    }
    return counters.WorkingSetSize;
}

void print_memory(const char* label) {
    std::cout << label << "_working_set_mb=" << std::fixed
              << std::setprecision(2)
              << static_cast<double>(working_set_bytes()) /
                     (1024.0 * 1024.0)
              << "\n";
}

std::filesystem::path default_snapshot_path() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer,
        static_cast<DWORD>(std::size(buffer)));
    std::filesystem::path root = length != 0 && length < std::size(buffer)
        ? std::filesystem::path(std::wstring(buffer, length))
        : std::filesystem::temp_directory_path();
    return root / L"everything_sm" / L"benchmarks" /
           L"mapped-catalog-benchmark.metadata";
}

std::vector<esm::FileRecord> generate(std::size_t count,
                                      std::size_t directory_count) {
    directory_count = std::min(directory_count, count);
    std::vector<esm::FileRecord> records;
    records.reserve(count);
    for (std::size_t i = 0; i < directory_count; ++i) {
        esm::FileRecord record;
        record.id = 2 + i;
        record.parent_id = root_id;
        record.directory = true;
        record.attributes = FILE_ATTRIBUTE_DIRECTORY;
        record.name = L"group_" + std::to_wstring(i);
        records.push_back(std::move(record));
    }
    for (std::size_t i = directory_count; i < count; ++i) {
        const auto group = (i - directory_count) % directory_count;
        esm::FileRecord record;
        record.id = 2 + i;
        record.parent_id = 2 + group;
        record.attributes = FILE_ATTRIBUTE_NORMAL;
        record.name = L"project_report_" + std::to_wstring(i) + L".txt";
        records.push_back(std::move(record));
    }
    return records;
}
} // namespace

int main(int argc, char** argv) {
    const std::size_t count = argc >= 2
        ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10))
        : 1'000'000;
    if (count < 2) {
        std::cerr << "record count must be at least 2\n";
        return 2;
    }
    const auto snapshot_path = argc >= 3
        ? std::filesystem::path(argv[2])
        : default_snapshot_path();
    const auto directory_count = std::min<std::size_t>(1'000, count / 2);
    std::cout << "records=" << count
              << " directories=" << directory_count
              << " snapshot=" << snapshot_path.string() << "\n";

    esm::MetadataSnapshot snapshot;
    snapshot.checkpoint = {123456789, 987654321};
    snapshot.root_id = root_id;
    snapshot.volume = L"D:";
    snapshot.records = generate(count, directory_count);
    print_memory("generated_input");

    const auto save_started = Clock::now();
    const auto saved = esm::save_metadata_snapshot_atomic(
        snapshot_path, snapshot);
    const auto save_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - save_started);
    if (!saved.ok) {
        std::cerr << "snapshot save failed error=" << saved.error << "\n";
        return 1;
    }
    std::cout << "snapshot_save_ms=" << save_ms.count()
              << " snapshot_mb=" << std::fixed << std::setprecision(2)
              << static_cast<double>(std::filesystem::file_size(snapshot_path)) /
                     (1024.0 * 1024.0)
              << "\n";

    std::vector<esm::FileRecord>().swap(snapshot.records);
    Sleep(50);
    print_memory("after_input_release");

    const auto load_started = Clock::now();
    auto loaded = esm::load_metadata_snapshot_mapped(snapshot_path);
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - load_started);
    if (!loaded.ok) {
        std::cerr << "mapped load failed error=" << loaded.error << "\n";
        return 1;
    }
    std::cout << "mapped_load_ms=" << load_ms.count()
              << " mapped_nodes=" << loaded.snapshot.catalog.node_count
              << " mapped_name_chars=" << loaded.snapshot.catalog.name_count
              << "\n";
    print_memory("mapped_loaded");

    esm::NtfsCatalog catalog(L"D:", root_id, 0);
    const auto attach_started = Clock::now();
    if (!catalog.replace_mapped(std::move(loaded.snapshot.catalog))) {
        std::cerr << "mapped catalog attach failed\n";
        return 1;
    }
    const auto attach_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - attach_started);
    const auto mapped_stats = catalog.storage_stats();
    std::cout << "mapped_attach_us=" << attach_us.count()
              << " base_nodes=" << mapped_stats.base_nodes
              << " compact_mb=" << std::fixed << std::setprecision(2)
              << static_cast<double>(mapped_stats.compact_storage_bytes) /
                     (1024.0 * 1024.0)
              << " mapped_file_mb="
              << static_cast<double>(mapped_stats.mapped_file_bytes) /
                     (1024.0 * 1024.0)
              << "\n";
    print_memory("mapped_catalog");

    esm::UsnChangeBatch changes;
    const auto change_count = std::min<std::size_t>(10'000,
                                                    count - directory_count);
    changes.changes.reserve(change_count);
    for (std::size_t i = 0; i < change_count; ++i) {
        const auto ordinal = directory_count + i;
        changes.changes.push_back({
            2 + ordinal,
            2 + (i % directory_count),
            static_cast<std::int64_t>(1000 + i),
            USN_REASON_BASIC_INFO_CHANGE,
            FILE_ATTRIBUTE_HIDDEN,
            L""
        });
    }
    const auto apply_started = Clock::now();
    const auto applied = catalog.apply(changes);
    const auto apply_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - apply_started);
    std::cout << "mapped_apply_ms="
              << static_cast<double>(apply_us.count()) / 1000.0
              << " upserts=" << applied.upserts.size()
              << " pending=" << catalog.pending_delta_size() << "\n";

    const auto compact_started = Clock::now();
    const bool compacted = catalog.compact();
    const auto compact_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - compact_started);
    const auto owned_stats = catalog.storage_stats();
    std::cout << "mapped_to_owned_compact=" << (compacted ? 1 : 0)
              << " compact_ms=" << compact_ms.count()
              << " mapped=" << (owned_stats.mapped_base ? 1 : 0)
              << " compact_mb=" << std::fixed << std::setprecision(2)
              << static_cast<double>(owned_stats.compact_storage_bytes) /
                     (1024.0 * 1024.0)
              << "\n";
    print_memory("owned_catalog");
    return 0;
}

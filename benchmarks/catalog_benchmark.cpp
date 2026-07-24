#include "esm/ntfs_catalog.hpp"
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
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
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return counters.WorkingSetSize;
}

void print_memory(const char* label) {
    std::cout << label << "_working_set_mb=" << std::fixed
              << std::setprecision(2)
              << static_cast<double>(working_set_bytes()) / (1024.0 * 1024.0)
              << "\n";
}

void print_storage(const esm::NtfsCatalog& catalog, const char* label) {
    const auto stats = catalog.storage_stats();
    std::cout << label
              << "_live=" << stats.live_nodes
              << " base=" << stats.base_nodes
              << " name_chars=" << stats.base_name_chars
              << " overlay=" << stats.overlay_nodes
              << " tombstones=" << stats.tombstones
              << " compact_mb=" << std::fixed << std::setprecision(2)
              << static_cast<double>(stats.compact_storage_bytes) /
                     (1024.0 * 1024.0)
              << " compactions=" << stats.compaction_count << "\n";
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
        record.path = L"D:\\" + record.name;
        records.push_back(std::move(record));
    }
    for (std::size_t i = directory_count; i < count; ++i) {
        const auto group = (i - directory_count) % directory_count;
        esm::FileRecord record;
        record.id = 2 + i;
        record.parent_id = 2 + group;
        record.attributes = FILE_ATTRIBUTE_NORMAL;
        record.name = L"project_report_" + std::to_wstring(i) + L".txt";
        record.path = L"D:\\group_" + std::to_wstring(group) +
                      L"\\" + record.name;
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
    const auto directory_count = std::min<std::size_t>(1'000, count / 2);
    std::cout << "records=" << count
              << " directories=" << directory_count << "\n";

    auto records = generate(count, directory_count);
    print_memory("generated_input");
    esm::NtfsCatalog catalog(L"D:", root_id);
    const auto build_start = Clock::now();
    catalog.replace(records);
    const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - build_start);
    std::cout << "catalog_build_ms=" << build_ms.count()
              << " size=" << catalog.size() << "\n";
    print_storage(catalog, "catalog_base");
    print_memory("catalog_with_input");

    // Isolate the catalog's steady-state footprint from the synthetic MFT
    // input vector, which owns a second copy of every name and full path.
    std::vector<esm::FileRecord>().swap(records);
    Sleep(50);
    print_memory("catalog_only");

    esm::UsnChangeBatch ordinary;
    ordinary.next_usn = 1000;
    const auto ordinary_count = std::min<std::size_t>(10'000,
                                                      count - directory_count);
    ordinary.changes.reserve(ordinary_count);
    for (std::size_t i = 0; i < ordinary_count; ++i) {
        const auto ordinal = directory_count + i;
        ordinary.changes.push_back({
            2 + ordinal,
            2 + (i % directory_count),
            static_cast<std::int64_t>(i + 1),
            USN_REASON_BASIC_INFO_CHANGE,
            FILE_ATTRIBUTE_NORMAL,
            L""
        });
    }
    const auto ordinary_start = Clock::now();
    const auto ordinary_result = catalog.apply(ordinary);
    const auto ordinary_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - ordinary_start);
    std::cout << "ordinary_changes=" << ordinary_count
              << " apply_ms=" << static_cast<double>(ordinary_us.count()) / 1000.0
              << " upserts=" << ordinary_result.upserts.size()
              << " removals=" << ordinary_result.removed_ids.size() << "\n";
    print_storage(catalog, "after_ordinary");

    esm::UsnChangeBatch rename;
    rename.next_usn = 2000;
    rename.changes.push_back({2, root_id, 1001,
                              USN_REASON_RENAME_OLD_NAME,
                              FILE_ATTRIBUTE_DIRECTORY, L"group_0"});
    rename.changes.push_back({2, root_id, 1002,
                              USN_REASON_RENAME_NEW_NAME,
                              FILE_ATTRIBUTE_DIRECTORY, L"renamed_group_0"});
    const auto rename_start = Clock::now();
    const auto rename_result = catalog.apply(rename);
    const auto rename_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - rename_start);
    std::cout << "directory_rename_ms=" << rename_ms.count()
              << " upserts=" << rename_result.upserts.size()
              << " renamed=" << rename_result.renamed << "\n";
    print_storage(catalog, "before_compaction");
    print_memory("before_compaction");

    const auto compact_start = Clock::now();
    const bool compacted = catalog.compact();
    const auto compact_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - compact_start);
    std::cout << "manual_compaction=" << (compacted ? 1 : 0)
              << " compact_ms=" << compact_ms.count() << "\n";
    print_storage(catalog, "after_compaction");
    print_memory("after_compaction");
    return 0;
}

#include "esm/index.hpp"
#include "esm/metadata_snapshot.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

std::size_t working_set_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                                sizeof(counters))
        ? counters.WorkingSetSize : 0;
}

void print_memory(const char* label) {
    std::cout << label << "_working_set_mb=" << std::fixed
              << std::setprecision(2)
              << static_cast<double>(working_set_bytes()) /
                     (1024.0 * 1024.0)
              << "\n";
}
} // namespace

int main() {
    const std::filesystem::path snapshot_path =
        L"C:\\ProgramData\\everything_sm\\indexes\\mft-index.snapshot";
    const auto load_started = Clock::now();
    auto loaded = esm::load_metadata_snapshot(snapshot_path);
    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - load_started);
    if (!loaded.ok) {
        std::cerr << "snapshot load failed error=" << loaded.error << "\n";
        return 1;
    }
    std::cout << "snapshot_records=" << loaded.snapshot.records.size()
              << " load_ms=" << load_ms.count() << "\n";
    print_memory("snapshot_loaded");

    esm::MetadataIndex index(0);
    const auto build_started = Clock::now();
    index.replace(loaded.snapshot.records);
    const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - build_started);
    std::vector<esm::FileRecord>().swap(loaded.snapshot.records);
    std::cout << "index_records=" << index.size()
              << " build_ms=" << build_ms.count() << "\n";
    print_memory("index_ready");

    const std::vector<std::wstring> queries = {
        L"1", L"12", L"123", L"txt", L"windows", L"report",
        L"123456789", L"123456789.txt", L"path:test1"
    };
    esm::SearchOptions options;
    options.limit = 100;
    options.match_path = false;
    for (const auto& query : queries) {
        std::vector<double> samples;
        std::size_t result_count = 0;
        for (int run = 0; run < 9; ++run) {
            const auto started = Clock::now();
            const auto results = index.search(query, options);
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::microseconds>(Clock::now() - started);
            samples.push_back(static_cast<double>(elapsed.count()) / 1000.0);
            result_count = results.size();
        }
        std::sort(samples.begin(), samples.end());
        std::wcout << L"query=\"" << query << L"\" results="
                   << result_count;
        std::cout << " p50_ms=" << samples[samples.size() / 2]
                  << " p95_ms=" << samples.back() << "\n";
    }
    return 0;
}

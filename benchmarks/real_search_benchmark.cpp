#include "esm/index.hpp"
#include "esm/metadata_snapshot.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct MemoryUsage {
    std::size_t working_set{};
    std::size_t private_bytes{};
};

MemoryUsage memory_usage() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return {};
    }
    return {counters.WorkingSetSize, counters.PrivateUsage};
}

void print_memory(const char* label) {
    const auto usage = memory_usage();
    std::cout << label << "_working_set_mb=" << std::fixed
              << std::setprecision(2)
              << static_cast<double>(usage.working_set) /
                     (1024.0 * 1024.0)
              << " " << label << "_private_mb="
              << static_cast<double>(usage.private_bytes) /
                     (1024.0 * 1024.0)
              << "\n";
}

class PeakMemorySampler {
public:
    PeakMemorySampler() {
        const auto initial = memory_usage();
        peak_working_set_.store(initial.working_set,
                                std::memory_order_relaxed);
        peak_private_.store(initial.private_bytes,
                            std::memory_order_relaxed);
        worker_ = std::thread([this] {
            while (!stop_.load(std::memory_order_relaxed)) {
                sample();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            sample();
        });
    }

    ~PeakMemorySampler() { stop(); }

    void stop() {
        if (!worker_.joinable()) return;
        stop_.store(true, std::memory_order_relaxed);
        worker_.join();
    }

    [[nodiscard]] std::size_t peak_working_set() const {
        return peak_working_set_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t peak_private() const {
        return peak_private_.load(std::memory_order_relaxed);
    }

private:
    static void update_peak(std::atomic<std::size_t>& peak,
                            std::size_t value) {
        auto current = peak.load(std::memory_order_relaxed);
        while (current < value &&
               !peak.compare_exchange_weak(current, value,
                                           std::memory_order_relaxed)) {
        }
    }

    void sample() {
        const auto usage = memory_usage();
        update_peak(peak_working_set_, usage.working_set);
        update_peak(peak_private_, usage.private_bytes);
    }

    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> peak_working_set_{0};
    std::atomic<std::size_t> peak_private_{0};
    std::thread worker_;
};
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
    PeakMemorySampler build_memory;
    const auto build_started = Clock::now();
    index.replace(std::move(loaded.snapshot.records));
    const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - build_started);
    build_memory.stop();
    std::cout << "index_records=" << index.size()
              << " build_ms=" << build_ms.count()
              << " build_peak_working_set_mb="
              << static_cast<double>(build_memory.peak_working_set()) /
                     (1024.0 * 1024.0)
              << " build_peak_private_mb="
              << static_cast<double>(build_memory.peak_private()) /
                     (1024.0 * 1024.0)
              << "\n";
    print_memory("index_ready");
    const auto stats = index.storage_stats();
    const auto mib = [](std::size_t bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    };
    std::cout << "name_only_paths=" << stats.name_only_paths
              << " record_mb=" << mib(stats.record_bytes)
              << " string_mb=" << mib(stats.string_bytes)
              << " signature_mb=" << mib(stats.signature_bytes)
              << " path_signature_count=" << stats.path_signature_count
              << " path_signature_owner_mb="
              << mib(stats.path_signature_owner_bytes)
              << " posting_mb=" << mib(stats.posting_bytes)
              << " posting_entries=" << stats.posting_entries
              << " posting_bytes_per_entry="
              << (stats.posting_entries == 0
                      ? 0.0
                      : static_cast<double>(stats.posting_bytes) /
                            static_cast<double>(stats.posting_entries))
              << " posting_compression_ratio="
              << (stats.posting_entries == 0
                      ? 0.0
                      : static_cast<double>(stats.posting_bytes) /
                            static_cast<double>(stats.posting_entries *
                                                sizeof(std::uint32_t)))
              << " ordering_mb=" << mib(stats.ordering_bytes)
              << " index_capacity_mb="
              << mib(stats.total_base_capacity_bytes) << "\n";

    const std::vector<std::wstring> queries = {
        L"1", L"12", L"123", L"txt", L"windows", L"report",
        L"123456789", L"123456789.txt", L"path:test1"
    };
    esm::SearchOptions options;
    options.limit = 1000;
    options.match_path = false;
    options.sort = esm::SortField::name;
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

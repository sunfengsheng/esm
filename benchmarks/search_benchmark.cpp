#include "esm/index.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

std::size_t working_set_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return counters.WorkingSetSize;
}

std::vector<esm::FileRecord> generate(std::size_t count) {
    std::vector<esm::FileRecord> records;
    records.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        esm::FileRecord record;
        record.id = i + 1;
        record.parent_id = i / 1000;
        record.directory = (i % 97) == 0;
        const std::wstring extension = (i % 5 == 0) ? L".md" : L".txt";
        record.name = L"project_report_" + std::to_wstring(i) + extension;
        record.path = L"D:\\benchmark\\group_" +
                      std::to_wstring(i / 1000) + L"\\" + record.name;
        records.push_back(std::move(record));
    }
    return records;
}

struct Delta {
    std::vector<esm::FileRecord> upserts;
    std::vector<std::uint64_t> removals;
};

Delta generate_delta(std::size_t begin, std::size_t end) {
    Delta delta;
    delta.upserts.reserve(end - begin);
    delta.removals.reserve((end - begin) / 5 + 1);
    for (std::size_t i = begin; i < end; ++i) {
        const auto id = static_cast<std::uint64_t>(i + 1);
        if ((i % 5) == 0) {
            delta.removals.push_back(id);
            continue;
        }
        esm::FileRecord record;
        record.id = id;
        record.parent_id = i / 1000;
        record.directory = false;
        const std::wstring extension = (i % 7 == 0) ? L".md" : L".txt";
        record.name = L"delta_report_" + std::to_wstring(i) + extension;
        record.path = L"D:\\benchmark\\delta_group_" +
                      std::to_wstring(i / 1000) + L"\\" + record.name;
        delta.upserts.push_back(std::move(record));
    }
    return delta;
}

void print_memory(const char* label) {
    std::cout << label << "_working_set_mb=" << std::fixed
              << std::setprecision(2)
              << static_cast<double>(working_set_bytes()) / (1024.0 * 1024.0)
              << "\n";
}

void run_queries(const esm::MetadataIndex& index, const std::string& phase) {
    const std::vector<std::wstring> queries = {
        L"1",
        L"12",
        L"123",
        L"txt",
        L"report",
        L"report_424242",
        L"ext:md report_999",
        L"path:group_777 project",
        L"project !9999",
        L"delta_report_99999"
    };
    esm::SearchOptions options;
    options.limit = 100;
    options.match_path = false;

    std::cout << "phase=" << phase << "\n";
    for (const auto& query : queries) {
        std::vector<double> samples;
        samples.reserve(9);
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
        std::wcout << L"  query=\"" << query << L"\" results="
                   << result_count;
        std::cout << " p50_ms=" << samples[samples.size() / 2]
                  << " p95_ms=" << samples.back() << "\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    const std::size_t count = argc >= 2
        ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10))
        : 1'000'000;
    std::cout << "records=" << count << "\n";

    auto records = generate(count);
    esm::MetadataIndex index(0); // Keep overlays for the staged benchmark.
    const auto build_start = Clock::now();
    index.replace(std::move(records));
    const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - build_start);
    std::cout << "build_ms=" << build_ms.count() << "\n";
    print_memory("base");
    run_queries(index, "base");

    std::vector<std::size_t> levels;
    for (const auto requested : {std::size_t{1'000}, std::size_t{10'000},
                                 std::size_t{100'000}}) {
        const auto level = std::min(requested, count);
        if (level != 0 && (levels.empty() || levels.back() != level)) {
            levels.push_back(level);
        }
    }

    std::size_t applied = 0;
    for (const auto level : levels) {
        auto delta = generate_delta(applied, level);
        const auto delta_size = delta.upserts.size() + delta.removals.size();
        const auto started = Clock::now();
        index.apply_delta(std::move(delta.upserts), delta.removals);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - started);
        std::cout << "delta_target=" << level
                  << " batch_changes=" << delta_size
                  << " apply_ms=" << static_cast<double>(elapsed.count()) / 1000.0
                  << " pending=" << index.pending_delta_size()
                  << " live=" << index.size() << "\n";
        print_memory(("overlay_" + std::to_string(level)).c_str());
        run_queries(index, "overlay_" + std::to_string(level));
        applied = level;
    }

    if (index.pending_delta_size() != 0) {
        const auto started = Clock::now();
        const bool compacted = index.compact();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started);
        std::cout << "manual_compaction=" << compacted
                  << " compact_ms=" << elapsed.count()
                  << " pending=" << index.pending_delta_size()
                  << " compactions=" << index.compaction_count() << "\n";
        print_memory("compacted");
        run_queries(index, "compacted");
    }

    if (count != 0) {
        const auto auto_count = std::min<std::size_t>(1'000, count);
        std::vector<esm::FileRecord> upserts;
        upserts.reserve(auto_count);
        for (std::size_t i = 0; i < auto_count; ++i) {
            esm::FileRecord record;
            record.id = i + 1;
            record.parent_id = 0;
            record.name = L"auto_compact_" + std::to_wstring(i) + L".txt";
            record.path = L"D:\\benchmark\\auto\\" + record.name;
            upserts.push_back(std::move(record));
        }
        index.set_auto_compaction_threshold(auto_count);
        const auto started = Clock::now();
        index.apply_delta(std::move(upserts), {});
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started);
        std::cout << "auto_compaction_changes=" << auto_count
                  << " apply_and_compact_ms=" << elapsed.count()
                  << " pending=" << index.pending_delta_size()
                  << " compactions=" << index.compaction_count() << "\n";
    }
    return 0;
}


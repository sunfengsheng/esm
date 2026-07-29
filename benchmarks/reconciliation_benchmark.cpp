#include "esm/index.hpp"
#include "esm/metadata_snapshot.hpp"
#include "esm/ntfs_enumerator.hpp"
#include "esm/volume_discovery.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct MemorySample {
    std::uint64_t working_set{};
    std::uint64_t private_bytes{};
};

MemorySample process_memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        return {};
    }
    return {static_cast<std::uint64_t>(counters.WorkingSetSize),
            static_cast<std::uint64_t>(counters.PrivateUsage)};
}

std::uint64_t elapsed_ms(Clock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started).count());
}

struct VolumeResult {
    esm::NtfsVolumeInfo volume;
    esm::ScanResult scan;
    std::size_t record_count{};
    std::uint64_t elapsed{};
};

std::wstring escape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const auto ch : value) {
        if (ch == L'\\' || ch == L'"') result.push_back(L'\\');
        result.push_back(ch);
    }
    return result;
}

std::wstring mib(std::uint64_t bytes) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(2)
        << static_cast<double>(bytes) / (1024.0 * 1024.0);
    return out.str();
}

void append_progress(const std::filesystem::path& path,
                     std::wstring_view stage,
                     std::size_t records = 0) {
    const auto memory = process_memory();
    std::wofstream output(path, std::ios::app);
    output << L"progress_stage=" << stage << L" records=" << records
           << L" working_set_mib=" << mib(memory.working_set)
           << L" private_mib=" << mib(memory.private_bytes) << L"\n";
    output.flush();
}
} // namespace

int main(int argc, char** argv) {
    const auto output_path = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::temp_directory_path() /
              L"everything_sm-reconciliation-benchmark.txt";
    const auto checkpoint_path = output_path.wstring() + L".snapshot";
    std::filesystem::create_directories(output_path.parent_path());
    {
        std::wofstream output(output_path, std::ios::trunc);
        output << L"benchmark=real-multi-volume-mft-reconciliation\n"
               << L"status=running\n";
    }
    append_progress(output_path, L"started");

    std::atomic_bool sampling{true};
    std::atomic_uint64_t peak_working_set{};
    std::atomic_uint64_t peak_private_bytes{};
    std::jthread sampler([&](std::stop_token token) {
        while (!token.stop_requested() &&
               sampling.load(std::memory_order_relaxed)) {
            const auto sample = process_memory();
            peak_working_set.store(
                (std::max)(peak_working_set.load(std::memory_order_relaxed),
                           sample.working_set),
                std::memory_order_relaxed);
            peak_private_bytes.store(
                (std::max)(peak_private_bytes.load(std::memory_order_relaxed),
                           sample.private_bytes),
                std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    const auto total_started = Clock::now();
    const auto discovery_started = Clock::now();
    const auto discovery = esm::discover_mounted_ntfs_volumes();
    const auto discovery_ms = elapsed_ms(discovery_started);
    append_progress(output_path, L"volume-discovery");
    if (discovery.error != ERROR_SUCCESS || discovery.volumes.empty()) {
        sampling.store(false, std::memory_order_relaxed);
        sampler.request_stop();
        std::wcerr << L"volume discovery failed: " << discovery.error << L'\n';
        return 2;
    }

    const auto enumeration_started = Clock::now();
    std::vector<std::future<VolumeResult>> futures;
    futures.reserve(discovery.volumes.size());
    for (auto volume : discovery.volumes) {
        futures.push_back(std::async(
            std::launch::async, [volume = std::move(volume)]() mutable {
                const auto started = Clock::now();
                VolumeResult result;
                result.volume = std::move(volume);
                result.scan = esm::enumerate_ntfs_volume(
                    result.volume.root,
                    {.hydrate_search_metadata = false});
                result.record_count = result.scan.records.size();
                result.elapsed = elapsed_ms(started);
                return result;
            }));
    }

    std::vector<VolumeResult> volumes;
    volumes.reserve(futures.size());
    std::size_t total_records = 0;
    std::size_t total_errors = 0;
    for (auto& future : futures) {
        auto volume = future.get();
        total_records += volume.scan.records.size();
        total_errors += volume.scan.errors;
        volumes.push_back(std::move(volume));
    }
    const auto enumeration_ms = elapsed_ms(enumeration_started);
    append_progress(output_path, L"mft-enumeration", total_records);
    if (total_records == 0 || total_errors != 0) {
        sampling.store(false, std::memory_order_relaxed);
        sampler.request_stop();
        std::wcerr << L"MFT enumeration incomplete: records=" << total_records
                   << L", errors=" << total_errors << L'\n';
        return 3;
    }

    const auto combine_started = Clock::now();
    std::vector<esm::FileRecord> records;
    records.reserve(total_records);
    for (auto& volume : volumes) {
        esm::namespace_ntfs_records(volume.volume.identity,
                                    volume.scan.records);
        records.insert(records.end(),
                       std::make_move_iterator(volume.scan.records.begin()),
                       std::make_move_iterator(volume.scan.records.end()));
        std::vector<esm::FileRecord>().swap(volume.scan.records);
    }
    const auto combine_ms = elapsed_ms(combine_started);
    const auto after_combine = process_memory();
    append_progress(output_path, L"combine-namespace", total_records);

    const auto index_started = Clock::now();
    esm::MetadataIndex index(0);
    index.replace(std::move(records));
    const auto index_ms = elapsed_ms(index_started);
    const auto after_index = process_memory();
    append_progress(output_path, L"index-build", total_records);

    const auto materialize_started = Clock::now();
    auto checkpoint_records = index.snapshot_records();
    const auto materialize_ms = elapsed_ms(materialize_started);
    const auto after_materialize = process_memory();
    append_progress(output_path, L"checkpoint-materialize", total_records);

    const auto save_started = Clock::now();
    esm::MetadataSnapshot snapshot;
    snapshot.checkpoint.journal_id = 1;
    snapshot.root_id = 1;
    snapshot.volume = L"everything_sm-mft-reconciliation-benchmark-v1";
    snapshot.records.swap(checkpoint_records);
    const auto saved = esm::save_metadata_snapshot_atomic(
        checkpoint_path, snapshot);
    snapshot.records.swap(checkpoint_records);
    const auto save_ms = elapsed_ms(save_started);
    const auto checkpoint_bytes = saved.ok
        ? std::filesystem::file_size(checkpoint_path) : 0;
    const auto after_save = process_memory();
    append_progress(output_path, L"checkpoint-save", total_records);

    sampling.store(false, std::memory_order_relaxed);
    sampler.request_stop();
    if (sampler.joinable()) sampler.join();

    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);

    std::wostringstream report;
    report << L"benchmark=real-multi-volume-mft-reconciliation\n"
           << L"timestamp_utc_filetime=";
    FILETIME timestamp{};
    GetSystemTimeAsFileTime(&timestamp);
    ULARGE_INTEGER timestamp_value{};
    timestamp_value.LowPart = timestamp.dwLowDateTime;
    timestamp_value.HighPart = timestamp.dwHighDateTime;
    report << timestamp_value.QuadPart << L"\n"
           << L"processor_count=" << system.dwNumberOfProcessors << L"\n"
           << L"physical_memory_mib=" << mib(memory.ullTotalPhys) << L"\n"
           << L"volume_count=" << volumes.size() << L"\n"
           << L"record_count=" << total_records << L"\n"
           << L"million_scale=" << (total_records >= 1'000'000 ? 1 : 0)
           << L"\n"
           << L"enumeration_errors=" << total_errors << L"\n";
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        report << L"volume." << i << L".root=\""
               << escape(volumes[i].volume.root) << L"\"\n"
               << L"volume." << i << L".identity=\""
               << escape(volumes[i].volume.identity) << L"\"\n"
               << L"volume." << i << L".records="
               << volumes[i].record_count << L"\n"
               << L"volume." << i << L".enumeration_ms="
               << volumes[i].elapsed << L"\n";
    }
    report << L"discovery_ms=" << discovery_ms << L"\n"
           << L"parallel_enumeration_ms=" << enumeration_ms << L"\n"
           << L"combine_namespace_ms=" << combine_ms << L"\n"
           << L"index_build_ms=" << index_ms << L"\n"
           << L"checkpoint_materialize_ms=" << materialize_ms << L"\n"
           << L"checkpoint_save_ms=" << save_ms << L"\n"
           << L"total_ms=" << elapsed_ms(total_started) << L"\n"
           << L"checkpoint_save_ok=" << (saved.ok ? 1 : 0) << L"\n"
           << L"checkpoint_save_error=" << saved.error << L"\n"
           << L"checkpoint_bytes=" << checkpoint_bytes << L"\n"
           << L"after_combine_working_set_mib="
           << mib(after_combine.working_set) << L"\n"
           << L"after_combine_private_mib="
           << mib(after_combine.private_bytes) << L"\n"
           << L"after_index_working_set_mib="
           << mib(after_index.working_set) << L"\n"
           << L"after_index_private_mib="
           << mib(after_index.private_bytes) << L"\n"
           << L"after_materialize_working_set_mib="
           << mib(after_materialize.working_set) << L"\n"
           << L"after_materialize_private_mib="
           << mib(after_materialize.private_bytes) << L"\n"
           << L"after_save_working_set_mib="
           << mib(after_save.working_set) << L"\n"
           << L"after_save_private_mib="
           << mib(after_save.private_bytes) << L"\n"
           << L"peak_working_set_mib="
           << mib(peak_working_set.load(std::memory_order_relaxed)) << L"\n"
           << L"peak_private_mib="
           << mib(peak_private_bytes.load(std::memory_order_relaxed)) << L"\n"
           << L"sampling_interval_ms=10\n"
           << L"scope=real local NTFS MFT enumeration plus in-process index "
              L"build and checkpoint materialization/save; no GUI or IPC\n";

    std::wofstream output(output_path, std::ios::trunc);
    output << report.str();
    output.close();
    std::wcout << report.str();

    std::error_code ignored;
    std::filesystem::remove(checkpoint_path, ignored);
    return saved.ok ? 0 : 4;
}

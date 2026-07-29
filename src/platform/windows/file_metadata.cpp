#include "esm/file_metadata.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace esm {
namespace {
std::wstring extended_path(std::wstring_view path) {
    if (path.starts_with(LR"(\\?\)") || path.starts_with(LR"(\\.\)"))
        return std::wstring(path);
    if (path.starts_with(LR"(\\)"))
        return LR"(\\?\UNC\)" + std::wstring(path.substr(2));
    if (path.size() >= 3 && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return LR"(\\?\)" + std::wstring(path);
    }
    return std::wstring(path);
}

std::int64_t file_time_value(const FILETIME& time) noexcept {
    return static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
        static_cast<std::uint64_t>(time.dwLowDateTime));
}
} // namespace

bool hydrate_file_search_metadata(FileRecord& record) noexcept {
    if (record.path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    const auto path = extended_path(record.path);
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &metadata))
        return false;

    record.attributes = metadata.dwFileAttributes;
    record.directory =
        (metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    record.last_write_time = file_time_value(metadata.ftLastWriteTime);
    record.size = record.directory
        ? 0
        : (static_cast<std::uint64_t>(metadata.nFileSizeHigh) << 32U) |
              static_cast<std::uint64_t>(metadata.nFileSizeLow);
    return true;
}

MetadataHydrationStats hydrate_file_search_metadata_records(
    std::span<FileRecord> records, std::size_t worker_count,
    std::span<std::uint8_t> succeeded) {
    MetadataHydrationStats result;
    result.attempted = records.size();
    if (!succeeded.empty() && succeeded.size() != records.size())
        throw std::invalid_argument("metadata success span size mismatch");
    std::fill(succeeded.begin(), succeeded.end(), std::uint8_t{});
    if (records.empty()) return result;

    if (worker_count == 0) {
        const auto available = std::thread::hardware_concurrency();
        worker_count = std::clamp<std::size_t>(available == 0 ? 1 : available,
                                               1, 4);
    } else {
        worker_count = std::clamp<std::size_t>(worker_count, 1, records.size());
    }

    std::atomic_size_t next{};
    std::atomic_size_t hydrated{};
    constexpr std::size_t claim_size = 64;
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            for (;;) {
                const auto begin =
                    next.fetch_add(claim_size, std::memory_order_relaxed);
                if (begin >= records.size()) break;
                const auto end = std::min(begin + claim_size, records.size());
                std::size_t local_hydrated = 0;
                for (auto index = begin; index < end; ++index) {
                    if (hydrate_file_search_metadata(records[index])) {
                        if (!succeeded.empty()) succeeded[index] = 1;
                        ++local_hydrated;
                    }
                }
                hydrated.fetch_add(local_hydrated,
                                   std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();

    result.hydrated = hydrated.load(std::memory_order_relaxed);
    result.errors = result.attempted - result.hydrated;
    return result;
}

bool hydrate_file_metadata(FileRecord& record) noexcept {
    if (record.path.empty()) return false;

    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    const auto path = extended_path(record.path);
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &metadata))
        return false;

    record.attributes = metadata.dwFileAttributes;
    record.directory =
        (metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    record.creation_time = file_time_value(metadata.ftCreationTime);
    record.last_access_time = file_time_value(metadata.ftLastAccessTime);
    record.last_write_time = file_time_value(metadata.ftLastWriteTime);

    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        FILE_BASIC_INFO basic{};
        if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                         sizeof(basic))) {
            record.change_time = basic.ChangeTime.QuadPart;
        }
        CloseHandle(handle);
    }
    record.size = record.directory
        ? 0
        : (static_cast<std::uint64_t>(metadata.nFileSizeHigh) << 32U) |
              static_cast<std::uint64_t>(metadata.nFileSizeLow);
    return true;
}
} // namespace esm

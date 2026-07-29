#pragma once
#include "esm/directory_scanner.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace esm {
struct NtfsEnumerationOptions {
    bool hydrate_search_metadata{true};
    std::size_t metadata_worker_count{};
};
[[nodiscard]] std::uint64_t query_ntfs_root_file_id(
    std::wstring_view volume) noexcept;
[[nodiscard]] ScanResult enumerate_ntfs_volume(
    std::wstring_view volume, NtfsEnumerationOptions options = {});
}

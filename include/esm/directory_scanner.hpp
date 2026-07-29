#pragma once
#include "esm/file_record.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
namespace esm {
struct ScanResult {
    std::vector<FileRecord> records;
    std::uint64_t root_id{};
    std::size_t errors{};
    std::size_t metadata_hydrated{};
    std::size_t metadata_errors{};
    std::chrono::milliseconds metadata_elapsed{};
    std::chrono::milliseconds elapsed{};
};
[[nodiscard]] ScanResult scan_directories(const std::vector<std::filesystem::path>& roots);
}

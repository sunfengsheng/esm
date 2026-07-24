#pragma once
#include "esm/file_record.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>
namespace esm {
struct ScanResult { std::vector<FileRecord> records; std::uint64_t root_id{}; std::size_t errors{}; std::chrono::milliseconds elapsed{}; };
[[nodiscard]] ScanResult scan_directories(const std::vector<std::filesystem::path>& roots);
}

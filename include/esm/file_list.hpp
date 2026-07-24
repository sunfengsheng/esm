#pragma once

#include "esm/file_record.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace esm {

// Everything-compatible EFU (Everything File List) reader/writer.
// Supported columns: Filename, Size, Date Modified, Attributes.
[[nodiscard]] bool load_efu_file(const std::filesystem::path& path,
                                 std::vector<FileRecord>& records,
                                 std::wstring& error);
[[nodiscard]] bool save_efu_file(const std::filesystem::path& path,
                                 std::span<const FileRecord> records,
                                 std::wstring& error);

} // namespace esm

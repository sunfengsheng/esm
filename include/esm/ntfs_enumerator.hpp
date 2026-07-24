#pragma once
#include "esm/directory_scanner.hpp"
#include <string_view>
namespace esm {
[[nodiscard]] ScanResult enumerate_ntfs_volume(std::wstring_view volume);
}

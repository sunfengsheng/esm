#pragma once

#include "esm/file_record.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esm {
struct NtfsVolumeInfo {
    std::wstring root;
    std::wstring mount_path;
    std::wstring identity;
    std::uint32_t serial_number{};
    bool removable{};
};

struct NtfsVolumeDiscoveryResult {
    std::vector<NtfsVolumeInfo> volumes;
    std::uint32_t error{};
};

// Discovers mounted local NTFS volumes that have drive-letter roots. Fixed
// disks are always included; removable NTFS volumes can optionally be added.
[[nodiscard]] NtfsVolumeDiscoveryResult discover_mounted_ntfs_volumes(
    bool include_removable = false);

// MFT file-reference numbers are only unique inside one volume. These helpers
// create stable process-independent IDs scoped by a volume GUID/identity so a
// single MetadataIndex can safely contain records from multiple volumes.
[[nodiscard]] std::uint64_t namespace_ntfs_file_id(
    std::wstring_view volume_identity,
    std::uint64_t file_id) noexcept;
void namespace_ntfs_records(std::wstring_view volume_identity,
                            std::vector<FileRecord>& records) noexcept;
void namespace_ntfs_file_ids(std::wstring_view volume_identity,
                             std::vector<std::uint64_t>& file_ids) noexcept;
} // namespace esm

#include "esm/volume_discovery.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <vector>

namespace esm {
namespace {
constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

void hash_byte(std::uint64_t& hash, unsigned char byte) noexcept {
    hash ^= byte;
    hash *= fnv_prime;
}

bool equals_ntfs(const wchar_t* filesystem) noexcept {
    return CompareStringOrdinal(filesystem, -1, L"NTFS", -1, TRUE) ==
           CSTR_EQUAL;
}

std::wstring volume_identity(const wchar_t* mount_path,
                             std::uint32_t serial_number) {
    std::array<wchar_t, 128> guid{};
    if (GetVolumeNameForVolumeMountPointW(
            mount_path, guid.data(), static_cast<DWORD>(guid.size()))) {
        return guid.data();
    }
    std::wstring fallback(mount_path);
    fallback += L"#" + std::to_wstring(serial_number);
    return fallback;
}
} // namespace

NtfsVolumeDiscoveryResult discover_mounted_ntfs_volumes(
    bool include_removable) {
    NtfsVolumeDiscoveryResult result;
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0) {
        result.error = GetLastError();
        return result;
    }

    std::vector<wchar_t> roots(static_cast<std::size_t>(required) + 1);
    const DWORD written = GetLogicalDriveStringsW(
        static_cast<DWORD>(roots.size()), roots.data());
    if (written == 0 || written >= roots.size()) {
        result.error = written == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return result;
    }

    for (const wchar_t* root = roots.data(); *root != L'\0';
         root += std::wcslen(root) + 1) {
        const UINT type = GetDriveTypeW(root);
        const bool removable = type == DRIVE_REMOVABLE;
        if (type != DRIVE_FIXED && !(include_removable && removable))
            continue;

        std::array<wchar_t, MAX_PATH + 1> filesystem{};
        DWORD serial_number = 0;
        if (!GetVolumeInformationW(
                root, nullptr, 0, &serial_number, nullptr, nullptr,
                filesystem.data(), static_cast<DWORD>(filesystem.size()))) {
            continue;
        }
        if (!equals_ntfs(filesystem.data())) continue;

        NtfsVolumeInfo info;
        info.mount_path = root;
        info.root.assign(root, root + 2);
        info.serial_number = serial_number;
        info.removable = removable;
        info.identity = volume_identity(root, serial_number);
        result.volumes.push_back(std::move(info));
    }

    std::sort(result.volumes.begin(), result.volumes.end(),
              [](const NtfsVolumeInfo& left, const NtfsVolumeInfo& right) {
                  return _wcsicmp(left.root.c_str(), right.root.c_str()) < 0;
              });
    result.error = ERROR_SUCCESS;
    return result;
}

std::uint64_t namespace_ntfs_file_id(
    std::wstring_view volume_identity,
    std::uint64_t file_id) noexcept {
    std::uint64_t hash = fnv_offset;
    for (wchar_t ch : volume_identity) {
        const auto folded = static_cast<std::uint16_t>(std::towupper(ch));
        hash_byte(hash, static_cast<unsigned char>(folded & 0xffU));
        hash_byte(hash, static_cast<unsigned char>((folded >> 8U) & 0xffU));
    }
    hash_byte(hash, 0xffU);
    for (unsigned shift = 0; shift < 64; shift += 8)
        hash_byte(hash, static_cast<unsigned char>(file_id >> shift));
    return hash == 0 ? 1 : hash;
}

void namespace_ntfs_records(std::wstring_view volume_identity,
                            std::vector<FileRecord>& records) noexcept {
    for (auto& record : records) {
        record.id = namespace_ntfs_file_id(volume_identity, record.id);
        record.parent_id =
            namespace_ntfs_file_id(volume_identity, record.parent_id);
    }
}

void namespace_ntfs_file_ids(
    std::wstring_view volume_identity,
    std::vector<std::uint64_t>& file_ids) noexcept {
    for (auto& file_id : file_ids)
        file_id = namespace_ntfs_file_id(volume_identity, file_id);
}
} // namespace esm

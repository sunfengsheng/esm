#include "esm/file_metadata.hpp"

#include <windows.h>

#include <string>
#include <string_view>

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

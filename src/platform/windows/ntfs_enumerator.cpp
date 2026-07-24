#include "esm/ntfs_enumerator.hpp"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
namespace esm {
namespace {
struct Handle { HANDLE value{INVALID_HANDLE_VALUE}; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
struct RawEntry { std::uint64_t id{}; std::uint64_t parent{}; std::uint32_t attributes{}; std::wstring name; };
std::wstring normalize_volume(std::wstring_view input) {
    if (input.size() < 2 || input[1] != L':') return {};
    wchar_t drive = input[0];
    if (drive >= L'a' && drive <= L'z') drive = static_cast<wchar_t>(drive - (L'a' - L'A'));
    return std::wstring{drive, L':'};
}
std::uint64_t root_file_id(const std::wstring& volume) {
    const std::wstring root = volume + L"\\";
    Handle handle{CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info)) return 0;
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
}
}
ScanResult enumerate_ntfs_volume(std::wstring_view requested_volume) {
    const auto started = std::chrono::steady_clock::now();
    ScanResult result;
    const std::wstring volume = normalize_volume(requested_volume);
    if (volume.empty()) { result.errors = 1; return result; }
    const std::wstring device = L"\\\\.\\" + volume;
    Handle handle{CreateFileW(device.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) { result.errors = 1; return result; }

    MFT_ENUM_DATA request{};
    request.StartFileReferenceNumber = 0;
    request.LowUsn = 0;
    request.HighUsn = std::numeric_limits<USN>::max();
    std::vector<unsigned char> buffer(1024 * 1024);
    std::vector<RawEntry> raw;
    for (;;) {
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(handle.value, FSCTL_ENUM_USN_DATA,
            &request, sizeof(request), buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr);
        if (!ok) {
            const DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF && error != ERROR_NO_MORE_FILES) ++result.errors;
            break;
        }
        if (returned <= sizeof(std::uint64_t)) break;
        request.StartFileReferenceNumber = *reinterpret_cast<const std::uint64_t*>(buffer.data());
        DWORD offset = sizeof(std::uint64_t);
        while (offset + sizeof(USN_RECORD) <= returned) {
            const auto* record = reinterpret_cast<const USN_RECORD*>(buffer.data() + offset);
            if (record->RecordLength == 0 || offset + record->RecordLength > returned) { ++result.errors; break; }
            if (record->MajorVersion == 2) {
                RawEntry entry;
                entry.id = record->FileReferenceNumber;
                entry.parent = record->ParentFileReferenceNumber;
                entry.attributes = record->FileAttributes;
                const auto* name = reinterpret_cast<const wchar_t*>(reinterpret_cast<const unsigned char*>(record) + record->FileNameOffset);
                entry.name.assign(name, record->FileNameLength / sizeof(wchar_t));
                raw.push_back(std::move(entry));
            }
            offset += record->RecordLength;
        }
    }

    const std::uint64_t root_id = root_file_id(volume);
    result.root_id = root_id;
    std::unordered_map<std::uint64_t, std::size_t> directories;
    directories.reserve(raw.size() / 8);
    for (std::size_t i = 0; i < raw.size(); ++i) if ((raw[i].attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) directories.emplace(raw[i].id, i);
    std::unordered_map<std::uint64_t, std::wstring> path_cache;
    path_cache.reserve(directories.size());
    path_cache.emplace(root_id, volume);
    std::function<std::wstring(std::uint64_t, unsigned)> resolve_directory = [&](std::uint64_t id, unsigned depth) -> std::wstring {
        if (id == root_id) return volume;
        if (depth > 512) return volume + L"\\$Cycle";
        if (const auto cached = path_cache.find(id); cached != path_cache.end()) return cached->second;
        const auto found = directories.find(id);
        if (found == directories.end()) return volume + L"\\$OrphanFiles";
        const auto& entry = raw[found->second];
        std::wstring parent = entry.parent == id ? volume : resolve_directory(entry.parent, depth + 1);
        std::wstring path = parent + L"\\" + entry.name;
        path_cache.emplace(id, path);
        return path;
    };

    result.records.reserve(raw.size());
    for (const auto& entry : raw) {
        if (entry.id == root_id) continue;
        FileRecord record;
        record.id = entry.id;
        record.parent_id = entry.parent;
        record.attributes = entry.attributes;
        record.directory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        record.name = entry.name;
        record.path = resolve_directory(entry.parent, 0) + L"\\" + entry.name;
        result.records.push_back(std::move(record));
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return result;
}
}

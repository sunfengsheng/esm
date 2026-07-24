#include "esm/journal_checkpoint.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace esm {
namespace {
constexpr std::array<unsigned char, 8> magic{'E', 'S', 'M', 'U', 'S', 'N', '1', 0};
constexpr std::uint32_t format_version = 1;
constexpr std::uint32_t corrupt_data_error = ERROR_INVALID_DATA;

struct CheckpointFile {
    std::array<unsigned char, 8> magic_bytes{};
    std::uint32_t version{};
    std::uint32_t reserved{};
    std::uint64_t journal_id{};
    std::int64_t next_usn{};
    std::uint64_t checksum{};
};
static_assert(sizeof(CheckpointFile) == 40);

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::uint64_t fnv1a64(const unsigned char* data, std::size_t size) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t checkpoint_checksum(const CheckpointFile& file) noexcept {
    return fnv1a64(reinterpret_cast<const unsigned char*>(&file),
                   offsetof(CheckpointFile, checksum));
}

std::filesystem::path temporary_path(const std::filesystem::path& path) {
    auto temp = path;
    temp += L".tmp";
    return temp;
}

bool volume_root(const std::filesystem::path& path, std::wstring& root,
                 std::uint32_t& error) {
    const DWORD full_size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (full_size == 0) {
        error = GetLastError();
        return false;
    }
    std::vector<wchar_t> full(static_cast<std::size_t>(full_size));
    if (GetFullPathNameW(path.c_str(), full_size, full.data(), nullptr) == 0) {
        error = GetLastError();
        return false;
    }

    std::vector<wchar_t> volume(MAX_PATH + 1);
    if (!GetVolumePathNameW(full.data(), volume.data(),
                            static_cast<DWORD>(volume.size()))) {
        error = GetLastError();
        return false;
    }
    root.assign(volume.data());
    error = ERROR_SUCCESS;
    return true;
}
} // namespace

PathVolumeComparison compare_path_volumes(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    PathVolumeComparison result;
    std::wstring left_root;
    std::wstring right_root;
    if (!volume_root(left, left_root, result.error) ||
        !volume_root(right, right_root, result.error)) {
        return result;
    }
    result.same_volume = CompareStringOrdinal(
        left_root.data(), static_cast<int>(left_root.size()),
        right_root.data(), static_cast<int>(right_root.size()), TRUE) ==
        CSTR_EQUAL;
    result.ok = true;
    return result;
}

CheckpointStatus validate_checkpoint(const UsnJournalState& journal,
                                     const JournalCheckpoint& checkpoint) noexcept {
    if (checkpoint.journal_id != journal.journal_id)
        return CheckpointStatus::journal_changed;
    if (checkpoint.next_usn < journal.first_usn ||
        checkpoint.next_usn < journal.lowest_valid_usn)
        return CheckpointStatus::expired;
    if (checkpoint.next_usn > journal.next_usn)
        return CheckpointStatus::ahead_of_journal;
    return CheckpointStatus::valid;
}

CheckpointLoadResult load_checkpoint(const std::filesystem::path& path) {
    CheckpointLoadResult result;
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    CheckpointFile data{};
    DWORD bytes_read = 0;
    if (!ReadFile(file.value, &data, sizeof(data), &bytes_read, nullptr)) {
        result.error = GetLastError();
        return result;
    }
    unsigned char trailing{};
    DWORD trailing_read = 0;
    if (bytes_read != sizeof(data) ||
        !ReadFile(file.value, &trailing, 1, &trailing_read, nullptr) ||
        trailing_read != 0 || data.magic_bytes != magic ||
        data.version != format_version || data.reserved != 0 ||
        data.checksum != checkpoint_checksum(data)) {
        result.error = corrupt_data_error;
        return result;
    }

    result.ok = true;
    result.checkpoint.journal_id = data.journal_id;
    result.checkpoint.next_usn = data.next_usn;
    return result;
}

CheckpointIoResult save_checkpoint_atomic(const std::filesystem::path& path,
                                          const JournalCheckpoint& checkpoint) {
    CheckpointIoResult result;
    std::error_code directory_error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) {
            result.error = static_cast<std::uint32_t>(directory_error.value());
            return result;
        }
    }

    CheckpointFile data{};
    data.magic_bytes = magic;
    data.version = format_version;
    data.journal_id = checkpoint.journal_id;
    data.next_usn = checkpoint.next_usn;
    data.checksum = checkpoint_checksum(data);

    const auto temp = temporary_path(path);
    Handle file{CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return result;
    }

    DWORD written = 0;
    if (!WriteFile(file.value, &data, sizeof(data), &written, nullptr) ||
        written != sizeof(data) || !FlushFileBuffers(file.value)) {
        result.error = GetLastError();
        return result;
    }
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;

    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        result.error = GetLastError();
        return result;
    }

    result.ok = true;
    return result;
}
} // namespace esm

#include "esm/directory_scanner.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>

namespace esm {
namespace {
constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

std::filesystem::path normalize_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::absolute(path, ec);
    if (ec) normalized = path;
    return normalized.lexically_normal();
}

std::wstring invariant_lower(std::wstring value) {
    if (value.empty()) return value;
    const auto ascii_fallback = [&] {
        for (auto& character : value) {
            if (character >= L'A' && character <= L'Z') {
                character = static_cast<wchar_t>(character - L'A' + L'a');
            }
        }
        return value;
    };

    const int required = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    if (required <= 0) return ascii_fallback();
    std::wstring mapped(static_cast<std::size_t>(required), L'\0');
    if (LCMapStringEx(
            LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
            static_cast<int>(value.size()), mapped.data(), required,
            nullptr, nullptr, 0) != required) {
        return ascii_fallback();
    }
    return mapped;
}

std::uint64_t stable_path_hash(const std::filesystem::path& path) {
    const auto normalized = invariant_lower(normalize_path(path).native());
    std::uint64_t hash = fnv_offset_basis;
    for (const wchar_t character : normalized) {
        const auto value = static_cast<std::uint16_t>(character);
        hash ^= static_cast<std::uint8_t>(value & 0xffu);
        hash *= fnv_prime;
        hash ^= static_cast<std::uint8_t>((value >> 8u) & 0xffu);
        hash *= fnv_prime;
    }
    return hash == 0 ? fnv_offset_basis : hash;
}

std::int64_t file_time_value(const FILETIME& value) noexcept {
    const auto result = (static_cast<std::uint64_t>(value.dwHighDateTime) << 32u) |
                        static_cast<std::uint64_t>(value.dwLowDateTime);
    return static_cast<std::int64_t>(result);
}

bool make_record(const std::filesystem::path& requested_path,
                 FileRecord& record, DWORD& attributes,
                 std::uint32_t& error) {
    const auto path = normalize_path(requested_path);
    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                              &metadata)) {
        error = GetLastError();
        return false;
    }

    attributes = metadata.dwFileAttributes;
    record.path = path.wstring();
    record.name = path.filename().wstring();
    record.id = stable_path_hash(path);
    record.parent_id = stable_path_hash(path.parent_path());
    record.attributes = attributes;
    record.directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    record.creation_time = file_time_value(metadata.ftCreationTime);
    record.last_access_time = file_time_value(metadata.ftLastAccessTime);
    record.last_write_time = file_time_value(metadata.ftLastWriteTime);
    // A recursive fallback scan has no MFT standard-information record, so
    // use last-write time as the best available change-time approximation.
    record.change_time = record.last_write_time;
    if (!record.directory) {
        record.size =
            (static_cast<std::uint64_t>(metadata.nFileSizeHigh) << 32u) |
            static_cast<std::uint64_t>(metadata.nFileSizeLow);
    }
    error = ERROR_SUCCESS;
    return true;
}
} // namespace

ScanResult scan_directories(const std::vector<std::filesystem::path>& roots) {
    const auto started = std::chrono::steady_clock::now();
    ScanResult result;
    if (roots.size() == 1) result.root_id = stable_path_hash(roots.front());

    for (const auto& requested_root : roots) {
        const auto root = normalize_path(requested_root);
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) {
            ++result.errors;
            continue;
        }

        const auto options =
            std::filesystem::directory_options::skip_permission_denied;
        std::vector<std::filesystem::path> pending_directories{root};
        while (!pending_directories.empty()) {
            auto directory = std::move(pending_directories.back());
            pending_directories.pop_back();

            std::filesystem::directory_iterator iterator(directory, options,
                                                           ec);
            const std::filesystem::directory_iterator end;
            if (ec) {
                ++result.errors;
                ec.clear();
                continue;
            }

            while (iterator != end) {
                FileRecord record;
                DWORD attributes = 0;
                std::uint32_t metadata_error = ERROR_SUCCESS;
                if (make_record(iterator->path(), record, attributes,
                                metadata_error)) {
                    if (record.directory &&
                        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                        pending_directories.push_back(record.path);
                    }
                    result.records.push_back(std::move(record));
                } else {
                    (void)metadata_error;
                    ++result.errors;
                }

                iterator.increment(ec);
                if (ec) {
                    ++result.errors;
                    ec.clear();
                    break;
                }
            }
        }
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}
} // namespace esm

